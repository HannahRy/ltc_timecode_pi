#ifndef LTC_COMMON_H
#define LTC_COMMON_H

#include <stdint.h>
#include <pthread.h>
#include <ltc.h>
#include <alsa/asoundlib.h>
#include <signal.h>
#include <math.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sched.h>

// Constants
#define SAMPLE_RATE 48000
#define DEFAULT_PCM_DEVICE "default"
#define CHANNELS 1
#define DEFAULT_CONFIG_FILE "/etc/ltc_timecode_pi.conf"
#define MAX_LINE 256
#define MICROSECONDS_PER_SECOND 1000000LL
#define NANOSECONDS_PER_MICROSECOND 1000LL

// Some libltc installs do not define LTC_TV_STANDARD, use int instead and define constants
#ifndef LTC_TV_525_60
#define LTC_TV_525_60 0
#endif
#ifndef LTC_TV_625_50
#define LTC_TV_625_50 1
#endif

// Structure for framerate specification
typedef struct {
    double fps;
    int std;         // Use int instead of LTC_TV_STANDARD for compatibility
    int drop_frame;
    const char* name;
} framerate_spec_t;

// Supported rates
extern const framerate_spec_t supported_rates[];
extern const int NUM_SUPPORTED_RATES;

// Shared state for timecode display thread
typedef struct {
    pthread_mutex_t lock;
    SMPTETimecode tc;         // Current timecode being displayed
    double fps;
    int drop_frame;
    int running;
    snd_pcm_t *pcm;           // PCM handle to query buffer state
    int64_t ntp_offset;       // Current NTP offset to apply
} timecode_display_state_t;

// Global variables that need to be shared
extern volatile sig_atomic_t running;
extern int use_ntp;
extern int64_t ntp_offset_us;
extern int64_t ntp_target_offset_us; 
extern pthread_mutex_t ntp_lock;

// Function declarations
void format_timecode(char *buf, size_t n, const SMPTETimecode *tc, double fps, int drop_frame);
void pin_to_core(int core_id);
void get_timecode_with_alsa_latency(SMPTETimecode *tc, double fps, snd_pcm_t *pcm, int drop_frame);
void get_display_timecode(SMPTETimecode *tc, double fps, int drop_frame, int64_t ntp_offset);
void set_realtime_priority(void);
int is_console_interactive(void);
const framerate_spec_t* parse_rate(const char* arg);
int configure_alsa_for_low_latency(snd_pcm_t *pcm, unsigned int rate, int ltc_frame_size);

// Thread functions
void* timecode_display_thread(void *arg);

#endif // LTC_COMMON_H