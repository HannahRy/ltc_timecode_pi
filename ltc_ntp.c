#include "ltc_ntp.h"
#include "ltc_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <inttypes.h>
#include <time.h>

// Global variables
char ntp_server[256] = "";
int64_t ntp_adjustment_step_us = 0;
int ntp_sync_interval = 60;     // Default sync interval in seconds (1 minute)
int ntp_slew_period = 30;       // Period over which to smear time adjustments in seconds

// Convert NTP format timestamp to Unix microseconds
int64_t ntp_to_unix_us(uint32_t ntp_sec, uint32_t ntp_frac) {
    // Convert seconds: NTP epoch (1900) to Unix epoch (1970)
    int64_t unix_sec = (int64_t)ntp_sec - NTP_TIMESTAMP_DELTA;
    
    // Convert fraction to microseconds (2^32 fractions = 1 second)
    int64_t us = ((int64_t)ntp_frac * MICROSECONDS_PER_SECOND) >> 32;
    
    return (unix_sec * MICROSECONDS_PER_SECOND) + us;
}

// Get current system time in NTP format
void get_system_time_ntp(uint32_t *sec, uint32_t *frac) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    
    *sec = (uint32_t)(ts.tv_sec + NTP_TIMESTAMP_DELTA);
    *frac = (uint32_t)((((uint64_t)ts.tv_nsec) << 32) / 1000000000LL);
}

// Perform a single NTP query and return the offset
int64_t perform_single_ntp_query(const char *hostname, int sockfd, struct sockaddr_in *server_addr) {
    ntp_packet packet = {0};
    
    // Set up NTP packet - Request version 4, mode client (3)
    packet.li_vn_mode = 0x23;  // LI = 0, VN = 4, Mode = 3
    
    // Record client transmit timestamp
    uint32_t tx_sec, tx_frac;
    struct timespec client_send_ts;
    
    // Get precise timestamp before sending
    clock_gettime(CLOCK_REALTIME, &client_send_ts);
    
    get_system_time_ntp(&tx_sec, &tx_frac);
    packet.tx_ts_sec = htonl(tx_sec);
    packet.tx_ts_frac = htonl(tx_frac);
    
    // Send request to server
    if (sendto(sockfd, &packet, sizeof(ntp_packet), 0, 
               (struct sockaddr *)server_addr, sizeof(*server_addr)) < 0) {
        perror("Error sending to NTP server");
        return INT64_MAX; // Signal error with large value
    }
    
    // Receive response from server
    socklen_t addr_len = sizeof(*server_addr);
    struct timespec client_recv_ts;
    
    if (recvfrom(sockfd, &packet, sizeof(ntp_packet), 0, 
                 (struct sockaddr *)server_addr, &addr_len) < 0) {
        perror("Error receiving from NTP server");
        return INT64_MAX; // Signal error with large value
    }
    
    // Get client receive timestamp immediately after receiving response
    clock_gettime(CLOCK_REALTIME, &client_recv_ts);
    
    // Convert NTP data from network byte order
    packet.tx_ts_sec = ntohl(packet.tx_ts_sec);
    packet.tx_ts_frac = ntohl(packet.tx_ts_frac);
    packet.recv_ts_sec = ntohl(packet.recv_ts_sec);
    packet.recv_ts_frac = ntohl(packet.recv_ts_frac);
    
    // Calculate server transmit time in Unix microseconds
    int64_t server_tx_us = ntp_to_unix_us(packet.tx_ts_sec, packet.tx_ts_frac);
    
    // Calculate client receive time in Unix microseconds
    int64_t client_recv_us = (int64_t)client_recv_ts.tv_sec * MICROSECONDS_PER_SECOND + 
                            client_recv_ts.tv_nsec / NANOSECONDS_PER_MICROSECOND;
    
    // Calculate offset: server_time - client_time
    return server_tx_us - client_recv_us;
}

// Query NTP server multiple times and use the shortest offset to reduce network latency impact
int query_ntp_server(const char *hostname) {
    int sockfd;
    struct sockaddr_in server_addr;
    struct hostent *server;
    struct timeval tv_timeout;
    
    // Set up timeout (5 seconds)
    tv_timeout.tv_sec = 5;
    tv_timeout.tv_usec = 0;
    
    // Create UDP socket
    sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        perror("Error creating socket");
        return -1;
    }
    
    // Set socket timeout
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv_timeout, sizeof(tv_timeout)) < 0) {
        perror("Error setting socket timeout");
        close(sockfd);
        return -1;
    }
    
    // Get server by name or IP
    server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr, "Error resolving NTP server: %s\n", hostname);
        close(sockfd);
        return -1;
    }
    
    // Set up server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(NTP_PORT);
    
    // Perform multiple queries and select the smallest offset
    int64_t min_offset = 0;
    int successful_queries = 0;
    int64_t sum_offset = 0;
    
    // Store all successful offsets for processing
    int64_t offsets[NTP_QUERY_COUNT];
    
    for (int i = 0; i < NTP_QUERY_COUNT; i++) {
        // Initialize to error value
        offsets[i] = INT64_MAX;
        
        // Perform the query
        int64_t offset = perform_single_ntp_query(hostname, sockfd, &server_addr);
        
        // Check if query was successful
        if (offset != INT64_MAX) {
            // Sanity check - ignore obviously wrong values (more than ±10 seconds)
            if (labs(offset) < NTP_ERROR_THRESHOLD) {
                offsets[i] = offset;
                successful_queries++;
                sum_offset += offset;
            }
            
            // Sleep between queries to avoid flooding server
            if (i < NTP_QUERY_COUNT - 1) {
                usleep(NTP_QUERY_INTERVAL);
            }
        }
    }
    
    // If we have successful queries, find the smallest valid offset
    if (successful_queries > 0) {
        // Start with average as fallback
        min_offset = sum_offset / successful_queries;
        
        // Then look for the minimum absolute offset 
        for (int i = 0; i < NTP_QUERY_COUNT; i++) {
            if (offsets[i] != INT64_MAX && labs(offsets[i]) < labs(min_offset)) {
                min_offset = offsets[i];
            }
        }
    }
    
    // Close socket
    close(sockfd);
    
    // If no successful queries, return error
    if (successful_queries == 0) {
        return -1;
    }
    
    // Update target offset with mutex protection
    pthread_mutex_lock(&ntp_lock);
    
    // Set target offset to the new NTP time offset (the minimum value found)
    // Double-check that the offset is reasonable before applying it
    if (labs(min_offset) < NTP_ERROR_THRESHOLD) {
        ntp_target_offset_us = min_offset;
    } else {
        // Log extreme values but don't apply them
        fprintf(stderr, "Warning: Ignoring extreme NTP offset value: %" PRId64 " microseconds\n", min_offset);
        pthread_mutex_unlock(&ntp_lock);
        return -1; // Consider this a failed sync
    }
    
    // Calculate how much to adjust per frame to reach target over slew period
    // Use the actual frame rate from the shared global variable if available
    // This is set in the main program based on the selected frame rate
    extern double selected_fps;  // Declare the external variable
    
    // Calculate number of frames over which to apply the adjustment
    int64_t adjust_frames = (int64_t)(ntp_slew_period * selected_fps);
    
    // Calculate adjustment per frame (how much to add to offset each frame)
    int64_t diff = ntp_target_offset_us - ntp_offset_us;
    if (adjust_frames > 0) {
        ntp_adjustment_step_us = diff / adjust_frames;
        // Ensure we have at least some adjustment if diff is small
        if (diff != 0 && ntp_adjustment_step_us == 0) {
            ntp_adjustment_step_us = (diff > 0) ? 1 : -1;
        }
    }
    
    pthread_mutex_unlock(&ntp_lock);
    
    return 0;
}

// Thread function for periodic NTP synchronization
void* ntp_sync_thread(void *arg) {
    ntp_thread_args_t *args = (ntp_thread_args_t*)arg;
    const char *server = args->server;
    int display_enabled = args->display_enabled;
    
    while (running) {
        // Sleep for configured interval before next sync
        for (int i = 0; i < ntp_sync_interval && running; i++) {
            sleep(1);
        }

        // Query NTP server
        if (query_ntp_server(server) == 0) {
            // Only show sync message if we're in interactive mode (not quiet)
            if (display_enabled) {
                printf(" NTP sync successful with server %s, target offset: %" PRId64 " microseconds\n", 
                    server, ntp_target_offset_us);
            }
        } else {
            fprintf(stderr, "NTP sync failed with server %s\n", server);
        }
    }
    
    free(arg); // Free allocated thread args
    return NULL;
}