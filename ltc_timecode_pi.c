/*
 * ALSA-paced, real-time LTC Timecode Generator using libltc
 * - Prints timecode to console as it runs (live updating, does not affect audio accuracy)
 * - Console output uses a low-priority thread to avoid interfering with real-time audio
 * - Supports all frame rates libltc supports (via command-line)
 * - Max volume, output is sample-accurate and ALSA buffer-latency compensated
 * - Pinned to CPU core 3 for deterministic scheduling
 * - Graceful exit on SIGINT/SIGTERM
 * - Real-time priority for audio thread
 * - Audio interface can be selected with -d or --device option
 * - Console timecode output is only shown when run directly (not as a systemd service or with --quiet)
 * - NTP synchronization with multiple-query best-offset selection
 *
 * Compile:
 *   gcc -pthread -o ltc_timecode_pi ltc_timecode_pi.c ltc_timecode.c ltc_ntp.c ltc_config.c -lltc -lasound -lm
 *
 * Usage:
 *   ./ltc_timecode_pi [-q] [-d device] [frame_rate]
 *   -q or --quiet: suppress console timecode output (recommended for daemon/service use)
 *   frame_rate: 24, 25, 29.97, 30, 29.97df, 30df (default: 25)
 *   -d or --device: ALSA device string (default: "default")
 *
 * To list available ALSA PCM devices, run:
 *   aplay -L
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <inttypes.h>
#include <sys/mman.h> // For mlockall

#include "ltc_common.h"
#include "ltc_ntp.h"
#include "ltc_config.h"

// Global variables required by header files
int use_ntp = 0;
int64_t ntp_offset_us = 0;
int64_t ntp_target_offset_us = 0;
pthread_mutex_t ntp_lock = PTHREAD_MUTEX_INITIALIZER;
double selected_fps = 25.0;  // Default frame rate, will be updated when actual rate is known

void handle_signal(int signo) {
    running = 0;
}

// Lock memory to prevent paging which can cause latency spikes
static void lock_memory(void) {
    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) {
        fprintf(stderr, "Warning: Failed to lock memory: %s\n", strerror(errno));
    } else {
        fprintf(stderr, "Memory locked successfully (prevents paging)\n");
    }
}

int main(int argc, char *argv[]) {
    // Default values
    const char *pcm_device = DEFAULT_PCM_DEVICE;
    const framerate_spec_t* rate = &supported_rates[1]; // Default: 25
    int quiet = 0;
    char config_file[PATH_MAX] = DEFAULT_CONFIG_FILE;

    // Option parsing
    int opt;
    int opt_index = 0;
    static struct option long_options[] = {
        {"quiet",  no_argument,       0, 'q'},
        {"device", required_argument, 0, 'd'},
        {"config", required_argument, 0,  0 },
        {"ntp-server", required_argument, 0, 0 },
        {"ntp-sync-interval", required_argument, 0, 0 },
        {"ntp-slew-period", required_argument, 0, 0 },
        {0, 0, 0, 0}
    };
    while ((opt = getopt_long(argc, argv, "qd:", long_options, &opt_index)) != -1) {
        if (opt == 0) {
            if (strcmp(long_options[opt_index].name, "config") == 0) {
                strncpy(config_file, optarg, sizeof(config_file)-1);
                config_file[sizeof(config_file)-1] = 0;
            } else if (strcmp(long_options[opt_index].name, "ntp-server") == 0) {
                strncpy(ntp_server, optarg, sizeof(ntp_server)-1);
                ntp_server[sizeof(ntp_server)-1] = 0;
                use_ntp = 1;
            } else if (strcmp(long_options[opt_index].name, "ntp-sync-interval") == 0) {
                ntp_sync_interval = atoi(optarg);
                if (ntp_sync_interval < 1) {
                    fprintf(stderr, "Warning: Invalid NTP sync interval, using default (60 seconds)\n");
                    ntp_sync_interval = 60;
                }
            } else if (strcmp(long_options[opt_index].name, "ntp-slew-period") == 0) {
                ntp_slew_period = atoi(optarg);
                if (ntp_slew_period < 1) {
                    fprintf(stderr, "Warning: Invalid NTP slew period, using default (30 seconds)\n");
                    ntp_slew_period = 30;
                }
            }
        } else switch (opt) {
            case 'd':
                pcm_device = optarg;
                break;
            case 'q':
                quiet = 1;
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    // Parse config file (if present)
    parse_config(config_file);
    // Use config values if not overridden by command line
    if (strcmp(pcm_device, DEFAULT_PCM_DEVICE) == 0 && strlen(config_device) > 0) {
        pcm_device = config_device;
    }
    if (optind < argc) {
        rate = parse_rate(argv[optind]);
        if (!rate) {
            print_usage(argv[0]);
            return 1;
        }
    } else if (strlen(config_framerate) > 0) {
        const framerate_spec_t* cfg_rate = parse_rate(config_framerate);
        if (cfg_rate) rate = cfg_rate;
    }
    
    // Update the global selected_fps variable with the actual frame rate
    selected_fps = rate->fps;

    // If not explicitly quiet and running interactively, show timecode display
    int show_timecode_display = !quiet && is_console_interactive();

    // Print startup info if quiet mode
    if (quiet) {
        printf("ltc_timecode_pi starting (quiet mode)\n");
        printf("PCM device: %s\n", pcm_device);
        printf("Frame rate: %s fps (%.3f), Drop Frame: %s\n",
            rate->name, rate->fps, rate->drop_frame ? "YES" : "NO");
        fflush(stdout);
    }

    // Signal handling for clean exit
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Default to core 3, but allow overriding via config
    int cpu_core = 3;
    char cpu_core_str[32] = "";
    
    // Check config for CPU core setting
    if (get_config_value("cpu-core", cpu_core_str, sizeof(cpu_core_str))) {
        cpu_core = atoi(cpu_core_str);
    }
    
    // Pin to specified CPU core (use -1 to disable)
    pin_to_core(cpu_core);
    
    // Lock memory to prevent paging which can cause latency spikes
    lock_memory();

    // ALSA setup
    snd_pcm_t *pcm;
    if (snd_pcm_open(&pcm, pcm_device, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "Failed to open PCM device '%s'\n", pcm_device);
        return 1;
    }

    // Calculate frame size for output FPS
    int ltc_frame_size = (int)round((double)SAMPLE_RATE / rate->fps);

    // Use our optimized ALSA configuration for low latency
    if (configure_alsa_for_low_latency(pcm, SAMPLE_RATE, ltc_frame_size) < 0) {
        fprintf(stderr, "Failed to configure ALSA for low latency\n");
        return 1;
    }

    LTCEncoder *encoder = ltc_encoder_create((double)SAMPLE_RATE, rate->fps, rate->std, rate->drop_frame);
    if (!encoder) {
        fprintf(stderr, "Failed to create LTC encoder\n");
        return 1;
    }

    int16_t *frame = (int16_t*)malloc(sizeof(int16_t) * ltc_frame_size);
    int8_t  *ltc_buf = (int8_t*)malloc(sizeof(int8_t) * ltc_frame_size);
    const int16_t max_amp = INT16_MAX;

    // Timecode display thread state
    timecode_display_state_t display;
    pthread_mutex_init(&display.lock, NULL);
    memset(&display.tc, 0, sizeof(SMPTETimecode));
    display.fps = rate->fps;
    display.drop_frame = rate->drop_frame;
    display.running = 1;

    // Start display thread if interactive
    pthread_t disp_thread;
    if (show_timecode_display) {
        // Pass PCM handle to display thread so it can display accurate timecode
        display.pcm = pcm;
        pthread_create(&disp_thread, NULL, timecode_display_thread, &display);
    }

    if (show_timecode_display) {
        printf("ALSA-paced LTC generator running on CPU core 3 with buffer latency compensation.\n");
        printf("PCM device: %s\n", pcm_device);
        printf("Frame rate: %s fps (%.3f), Drop Frame: %s\n",
            rate->name, rate->fps, rate->drop_frame ? "YES" : "NO");
        printf("Ctrl+C to stop.\n");
    }

    // Set real-time priority for audio (main) thread
    set_realtime_priority();
    
    // Start NTP synchronization thread if a server is specified
    pthread_t ntp_thread;
    if (use_ntp && strlen(ntp_server) > 0) {
        if (show_timecode_display) {
            printf("Using NTP server: %s for timecode synchronization\n", ntp_server);
        }
        // Initial NTP sync
        if (query_ntp_server(ntp_server) == 0) {
            if (show_timecode_display) {
                printf("Initial NTP sync successful with server %s, target offset: %" PRId64 " microseconds\n", 
                       ntp_server, ntp_target_offset_us);
            }
        } else {
            fprintf(stderr, "Initial NTP sync failed with server %s\n", ntp_server);
        }
        
        // Set up arguments for NTP sync thread
        ntp_thread_args_t *ntp_args = malloc(sizeof(ntp_thread_args_t));
        if (ntp_args == NULL) {
            fprintf(stderr, "Failed to allocate memory for NTP thread arguments\n");
            return 1;
        }
        ntp_args->server = ntp_server;
        ntp_args->display_enabled = show_timecode_display;
        
        // Start thread for periodic NTP sync
        pthread_create(&ntp_thread, NULL, ntp_sync_thread, ntp_args);
    }

    // Main loop: output LTC to ALSA, update display state
    while (running) {
        SMPTETimecode tc;
        get_timecode_with_alsa_latency(&tc, rate->fps, pcm, rate->drop_frame);
        ltc_encoder_set_timecode(encoder, &tc);
        ltc_encoder_encode_frame(encoder);

        // Suppress deprecated warning for ltc_encoder_get_buffer
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        ltc_encoder_get_buffer(encoder, (ltcsnd_sample_t*)ltc_buf);
        #pragma GCC diagnostic pop

        for (int i = 0; i < ltc_frame_size; ++i) {
            float s = ltc_buf[i] / 127.0f;
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            frame[i] = (int16_t)(s * max_amp);
        }

        int written = snd_pcm_writei(pcm, frame, ltc_frame_size);
        if (written < 0) {
            if (!running) break; // allow clean exit
            snd_pcm_recover(pcm, written, 1);
            snd_pcm_prepare(pcm);
            continue;
        }

        // Display updates are now handled by the display thread
    }

    // Cleanup
    display.running = 0;
    if (show_timecode_display) {
        pthread_join(disp_thread, NULL);
    }
    
    // Wait for NTP thread if it was started
    if (use_ntp && strlen(ntp_server) > 0) {
        pthread_join(ntp_thread, NULL);
    }
    
    ltc_encoder_free(encoder);
    free(frame);
    free(ltc_buf);
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    pthread_mutex_destroy(&display.lock);
    pthread_mutex_destroy(&ntp_lock);
    
    if (show_timecode_display) {
        printf("Exited gracefully.\n");
    }
    return 0;
}