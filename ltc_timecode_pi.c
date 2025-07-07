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
 *
 * Compile:
 *   gcc -pthread -o ltc_timecode_pi ltc_timecode_pi.c -lltc -lasound -lm
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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sched.h>
#include <time.h>
#include <string.h>
#include <ltc.h>
#include <alsa/asoundlib.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

// Some libltc installs do not define LTC_TV_STANDARD, use int instead and define constants
#ifndef LTC_TV_525_60
#define LTC_TV_525_60 0
#endif
#ifndef LTC_TV_625_50
#define LTC_TV_625_50 1
#endif

#define SAMPLE_RATE 48000
#define DEFAULT_PCM_DEVICE "default"
#define CHANNELS 1
#define DEFAULT_CONFIG_FILE "/etc/ltc_timecode_pi.conf"
#define MAX_LINE 256

typedef struct {
    double fps;
    int std;         // Use int instead of LTC_TV_STANDARD for compatibility
    int drop_frame;
    const char* name;
} framerate_spec_t;

// Supported rates
static const framerate_spec_t supported_rates[] = {
    {24.0,    LTC_TV_525_60, 0, "24"},
    {25.0,    LTC_TV_625_50, 0, "25"},
    {29.97,   LTC_TV_525_60, 0, "29.97"},
    {30.0,    LTC_TV_525_60, 0, "30"},
    {29.97,   LTC_TV_525_60, 1, "29.97df"},
    {30.0,    LTC_TV_525_60, 1, "30df"}
};

#define NUM_SUPPORTED_RATES (sizeof(supported_rates)/sizeof(supported_rates[0]))

// Shared state for timecode display thread
typedef struct {
    pthread_mutex_t lock;
    SMPTETimecode tc;
    double fps;
    int drop_frame;
    int running;
} timecode_display_state_t;

volatile sig_atomic_t running = 1;

void handle_signal(int signo) {
    running = 0;
}

void format_timecode(char *buf, size_t n, const SMPTETimecode *tc, double fps, int drop_frame) {
    if (drop_frame) {
        snprintf(buf, n, "\r%02d:%02d:%02d;%02d @ %.3f fps",
            tc->hours, tc->mins, tc->secs, tc->frame, fps);
    } else {
        snprintf(buf, n, "\r%02d:%02d:%02d:%02d @ %.3f fps",
            tc->hours, tc->mins, tc->secs, tc->frame, fps);
    }
}

// Pin process to CPU core (core_id is 0-based)
void pin_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

// Print usage and supported rates
void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [-q] [-d device] [frame_rate]\n", prog);
    fprintf(stderr, "  -q, --quiet    Suppress console timecode output (recommended for service)\n");
    fprintf(stderr, "  -d, --device   ALSA PCM device string (default: \"default\")\n");
    fprintf(stderr, "Supported frame rates:\n");
    for (size_t i = 0; i < NUM_SUPPORTED_RATES; ++i) {
        fprintf(stderr, "  %s\n", supported_rates[i].name);
    }
}

// Find framerate_spec_t from arg, or NULL if not found
const framerate_spec_t* parse_rate(const char* arg) {
    for (size_t i = 0; i < NUM_SUPPORTED_RATES; ++i) {
        if (strcmp(arg, supported_rates[i].name) == 0)
            return &supported_rates[i];
    }
    return NULL;
}

// Fill SMPTETimecode from adjusted system clock (with ALSA buffer delay compensation)
void get_timecode_with_alsa_latency(SMPTETimecode *tc, double fps, snd_pcm_t *pcm, int drop_frame) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    // Query output latency in frames
    snd_pcm_sframes_t delay_frames = 0;
    if (snd_pcm_delay(pcm, &delay_frames) < 0) {
        delay_frames = 0;
    }
    double buffer_delay_sec = delay_frames / (double)SAMPLE_RATE;
    // Adjust system time by buffer latency
    double adj_sec = ts.tv_sec + ts.tv_nsec / 1e9 + buffer_delay_sec;
    time_t adj_whole = (time_t)adj_sec;
    double adj_frac = adj_sec - adj_whole;
    struct tm *tm = localtime(&adj_whole);

    tc->years   = tm->tm_year + 1900;
    tc->months  = tm->tm_mon + 1;
    tc->days    = tm->tm_mday;
    tc->hours   = tm->tm_hour;
    tc->mins    = tm->tm_min;
    tc->secs    = tm->tm_sec;

    // SMPTE drop-frame for 29.97/30DF: frames 0 and 1 are dropped at the top of each minute except every tenth
    int frame = (int)round(adj_frac * fps);

    if (drop_frame) {
        int d = 2; // always 2 frames dropped per minute
        if ((tc->mins % 10) != 0 && frame < d) {
            frame = d;
        }
        if (frame >= (int)fps) frame = (int)fps - 1;
    } else {
        if (frame >= (int)fps) frame = (int)fps - 1;
    }
    tc->frame = frame;
}

// Low-priority thread to display timecode on the console
void* timecode_display_thread(void *arg) {
    timecode_display_state_t *display = (timecode_display_state_t*)arg;

#ifdef SCHED_IDLE
    struct sched_param param;
    param.sched_priority = 0;
    pthread_setschedparam(pthread_self(), SCHED_IDLE, &param);
#endif

    char buf[80];
    SMPTETimecode last_tc = {0};

    while (display->running) {
        pthread_mutex_lock(&display->lock);
        if (memcmp(&display->tc, &last_tc, sizeof(SMPTETimecode)) != 0) {
            format_timecode(buf, sizeof(buf), &display->tc, display->fps, display->drop_frame);
            fwrite(buf, 1, strlen(buf), stdout);
            fflush(stdout);
            last_tc = display->tc;
        }
        pthread_mutex_unlock(&display->lock);
        usleep(1000); // 1ms
    }
    printf("\n");
    return NULL;
}

void set_realtime_priority(void) {
    struct sched_param sp;
    sp.sched_priority = 20; // 1-99 (99=highest), 20 is safe for non-root, adjust as needed
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        fprintf(stderr, "Warning: Failed to set real-time priority for audio thread: %s\n", strerror(errno));
    }
}

// Return 1 if attached to a terminal, 0 otherwise
int is_console_interactive(void) {
    // Only consider interactive if stdout is a tty and not running under systemd
    return isatty(STDOUT_FILENO) && (getenv("INVOCATION_ID") == NULL);
}

char config_device[128] = "";
char config_framerate[32] = "";

void parse_config(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line;
        char *val = eq + 1;
        // Remove trailing newline
        val[strcspn(val, "\r\n")] = 0;
        if (strcmp(key, "device") == 0) {
            strncpy(config_device, val, sizeof(config_device)-1);
        } else if (strcmp(key, "framerate") == 0) {
            strncpy(config_framerate, val, sizeof(config_framerate)-1);
        }
    }
    fclose(f);
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
        {0, 0, 0, 0}
    };
    while ((opt = getopt_long(argc, argv, "qd:", long_options, &opt_index)) != -1) {
        if (opt == 0 && strcmp(long_options[opt_index].name, "config") == 0) {
            strncpy(config_file, optarg, sizeof(config_file)-1);
            config_file[sizeof(config_file)-1] = 0;
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

    pin_to_core(3);

    // ALSA setup
    snd_pcm_t *pcm;
    if (snd_pcm_open(&pcm, pcm_device, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "Failed to open PCM device '%s'\n", pcm_device);
        return 1;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm, params);
    snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, params, CHANNELS);
    unsigned int alsa_rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(pcm, params, &alsa_rate, 0);

    // Calculate frame size for output FPS
    int ltc_frame_size = (int)round((double)SAMPLE_RATE / rate->fps);

    // Large buffer and period for reliability
    snd_pcm_uframes_t buffer_size = ltc_frame_size * 32;
    snd_pcm_uframes_t period_size = ltc_frame_size;
    snd_pcm_hw_params_set_buffer_size_near(pcm, params, &buffer_size);
    snd_pcm_hw_params_set_period_size_near(pcm, params, &period_size, 0);

    if (snd_pcm_hw_params(pcm, params) < 0) {
        fprintf(stderr, "Failed to set PCM hardware parameters\n");
        return 1;
    }
    snd_pcm_prepare(pcm);

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

        // Update timecode for display (non-blocking)
        if (show_timecode_display) {
            pthread_mutex_lock(&display.lock);
            display.tc = tc;
            pthread_mutex_unlock(&display.lock);
        }
    }

    // Cleanup
    display.running = 0;
    if (show_timecode_display) {
        pthread_join(disp_thread, NULL);
    }
    ltc_encoder_free(encoder);
    free(frame);
    free(ltc_buf);
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    pthread_mutex_destroy(&display.lock);
    if (show_timecode_display) {
        printf("Exited gracefully.\n");
    }
    return 0;
}