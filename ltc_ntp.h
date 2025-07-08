#ifndef LTC_NTP_H
#define LTC_NTP_H

#include <stdint.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "ltc_common.h"

#define NTP_PORT 123
#define NTP_TIMESTAMP_DELTA 2208988800LL // Seconds between 1900 (NTP epoch) and 1970 (Unix epoch)
#define NTP_QUERY_COUNT 5   // Number of NTP queries to perform for each sync
#define NTP_QUERY_INTERVAL 200000 // Microseconds between queries (200ms)
#define NTP_ERROR_THRESHOLD (10 * MICROSECONDS_PER_SECOND) // 10 seconds in microseconds

// NTP packet structure according to RFC 5905
typedef struct {
    uint8_t li_vn_mode;      // Leap indicator, version and mode
    uint8_t stratum;         // Stratum level
    uint8_t poll;            // Poll interval
    uint8_t precision;       // Precision
    uint32_t root_delay;     // Root delay
    uint32_t root_dispersion; // Root dispersion
    uint32_t ref_id;         // Reference ID
    uint32_t ref_ts_sec;     // Reference timestamp seconds
    uint32_t ref_ts_frac;    // Reference timestamp fraction
    uint32_t orig_ts_sec;    // Origin timestamp seconds
    uint32_t orig_ts_frac;   // Origin timestamp fraction
    uint32_t recv_ts_sec;    // Receive timestamp seconds
    uint32_t recv_ts_frac;   // Receive timestamp fraction
    uint32_t tx_ts_sec;      // Transmit timestamp seconds
    uint32_t tx_ts_frac;     // Transmit timestamp fraction
} ntp_packet;

// Structure for NTP thread arguments 
typedef struct {
    const char *server;
    int display_enabled;
} ntp_thread_args_t;

// Global variables related to NTP
extern char ntp_server[256];
extern int64_t ntp_adjustment_step_us;
extern int ntp_sync_interval;
extern int ntp_slew_period;
extern int64_t ntp_offset_us;
extern int64_t ntp_target_offset_us;

// Function declarations
int64_t ntp_to_unix_us(uint32_t ntp_sec, uint32_t ntp_frac);
void get_system_time_ntp(uint32_t *sec, uint32_t *frac);
int64_t perform_single_ntp_query(const char *hostname, int sockfd, struct sockaddr_in *server_addr);
int query_ntp_server(const char *hostname);
void* ntp_sync_thread(void *arg);

#endif // LTC_NTP_H