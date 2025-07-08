#include "ltc_common.h"
#include "ltc_ntp.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <inttypes.h>
#include <errno.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Global variables
volatile sig_atomic_t running = 1;

// Supported rates definition
const framerate_spec_t supported_rates[] = {
    {24.0,    LTC_TV_525_60, 0, "24"},
    {25.0,    LTC_TV_625_50, 0, "25"},
    {29.97,   LTC_TV_525_60, 0, "29.97"},
    {30.0,    LTC_TV_525_60, 0, "30"},
    {29.97,   LTC_TV_525_60, 1, "29.97df"},
    {30.0,    LTC_TV_525_60, 1, "30df"}
};

const int NUM_SUPPORTED_RATES = sizeof(supported_rates)/sizeof(supported_rates[0]);

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
    if (core_id < 0) return;  // Allow disabling CPU pinning via config
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0) {
        fprintf(stderr, "Warning: Failed to pin process to CPU core %d: %s\n", 
                core_id, strerror(errno));
    }
}

// Fill SMPTETimecode from adjusted system clock (with ALSA buffer delay compensation)
// Using 64-bit fixed-point arithmetic with microsecond precision
void get_timecode_with_alsa_latency(SMPTETimecode *tc, double fps, snd_pcm_t *pcm, int drop_frame) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    // Convert to microseconds (64-bit integer)
    int64_t time_us = (int64_t)ts.tv_sec * MICROSECONDS_PER_SECOND + 
                      (int64_t)(ts.tv_nsec / NANOSECONDS_PER_MICROSECOND);
    
    // Apply NTP offset if enabled
    if (use_ntp) {
        pthread_mutex_lock(&ntp_lock);
        
        // Apply current offset
        time_us += ntp_offset_us;
        
        // Adjust the offset gradually toward target with each frame
        if (ntp_offset_us != ntp_target_offset_us && ntp_adjustment_step_us != 0) {
            ntp_offset_us += ntp_adjustment_step_us;
            
            // Check if we've reached or overshot the target
            if ((ntp_adjustment_step_us > 0 && ntp_offset_us >= ntp_target_offset_us) || 
                (ntp_adjustment_step_us < 0 && ntp_offset_us <= ntp_target_offset_us)) {
                ntp_offset_us = ntp_target_offset_us;  // We've reached the target
                ntp_adjustment_step_us = 0;            // Stop adjusting
            }
        }
        
        pthread_mutex_unlock(&ntp_lock);
    }

    // Query accurate output latency information
    snd_pcm_sframes_t delay_frames = 0;
    snd_pcm_status_t *status;
    snd_pcm_status_alloca(&status);
    
    // Get detailed PCM status
    if (snd_pcm_status(pcm, status) >= 0) {
        // Get delay in frames - this includes both hardware and software buffers
        delay_frames = snd_pcm_status_get_delay(status);
        
        // Ensure delay is non-negative
        if (delay_frames < 0) {
            delay_frames = 0;
        }
    } else {
        // Fallback to simpler delay function if status call fails
        if (snd_pcm_delay(pcm, &delay_frames) < 0) {
            delay_frames = 0;
        }
        if (delay_frames < 0) {
            delay_frames = 0;
        }
    }
    
    // Convert delay to microseconds with high precision
    // Use 64-bit arithmetic throughout to avoid overflows and maximize precision
    int64_t buffer_delay_us = (delay_frames * MICROSECONDS_PER_SECOND + (SAMPLE_RATE / 2)) / SAMPLE_RATE;
    
    // Calculate frame duration in microseconds
    int64_t frame_us = 0;
    if (fps == 29.97) {
        frame_us = MICROSECONDS_PER_SECOND * 1001 / 30000; // Exact for NTSC
    } else if (fps == 23.976) {
        frame_us = MICROSECONDS_PER_SECOND * 1001 / 24000; // Exact for 23.976
    } else {
        frame_us = (int64_t)(MICROSECONDS_PER_SECOND / fps);
    }
    
    // Calculate frame fraction within the current second (0.0 to 1.0)
    double second_fraction = (double)(ts.tv_nsec) / 1000000000.0;
    
    // Adaptive timing correction - more at start of second, less at end
    // This helps address the phenomenon where start of second has more delay
    // At the start of a second (second_fraction near 0), apply max correction
    // At the end of a second (second_fraction near 1), apply min correction
    int64_t min_frames_offset = 1.0;  // Minimum frames offset (near end of second)
    int64_t max_frames_offset = 3.5;  // Maximum frames offset (near start of second) - increased to compensate for 3-frame delay
    
    // Use a non-linear curve for better adaptation - exponential decay curve
    // This provides more correction at the beginning of the second and
    // approaches the minimum correction more quickly toward the end
    double decay_rate = 3.0;  // Higher value for faster transition to lower correction
    double normalized_position = 1.0 - exp(-decay_rate * second_fraction);
    double offset_frames = max_frames_offset - (normalized_position * (max_frames_offset - min_frames_offset));
    
    // Add a small phase adjustment term to fine-tune the timing
    // Sine wave with period of 1 second adds a gentle oscillation that can help with alignment
    double phase_adjustment = 0.2 * sin(2 * M_PI * second_fraction);
    offset_frames += phase_adjustment;
    
    // Add a slight quadratic component to further enhance start-of-second correction
    offset_frames += 0.3 * (1.0 - second_fraction * second_fraction);
    
    // Calculate processing offset in microseconds
    int64_t processing_offset_us = (int64_t)(frame_us * offset_frames);
    
    // Adjust time by buffer latency plus processing offset (microseconds)
    int64_t adj_time_us = time_us + buffer_delay_us + processing_offset_us;
    
    // Convert back to seconds and fraction for localtime
    time_t adj_whole = (time_t)(adj_time_us / MICROSECONDS_PER_SECOND);
    int64_t adj_frac_us = adj_time_us % MICROSECONDS_PER_SECOND;
    
    struct tm *tm = localtime(&adj_whole);

    tc->years   = tm->tm_year + 1900;
    tc->months  = tm->tm_mon + 1;
    tc->days    = tm->tm_mday;
    tc->hours   = tm->tm_hour;
    tc->mins    = tm->tm_min;
    tc->secs    = tm->tm_sec;

    // Calculate frame using precise frame boundaries to ensure frame 0 aligns with second rollover
    // First convert fps to a rational number for fixed-point math
    int64_t frame_numerator, frame_denominator;
    
    if (fps == 29.97) {
        // Use exact fraction for NTSC rates (30000/1001)
        frame_numerator = 30000;
        frame_denominator = 1001;
    } else if (fps == 23.976) {
        frame_numerator = 24000;
        frame_denominator = 1001;
    } else {
        // For integer rates, use simple conversion
        frame_numerator = (int64_t)(fps * 1000);
        frame_denominator = 1000;
    }
    
    // Calculate microseconds per frame (exactly)
    int64_t us_per_frame = (MICROSECONDS_PER_SECOND * frame_denominator) / frame_numerator;
    
    // Calculate frame number by determining how many complete frames fit in the fractional part
    int frame = (int)(adj_frac_us / us_per_frame);
    
    // Ensure perfect alignment - frame 0 must start exactly at second boundary
    // Adjust for any rounding errors
    if (frame >= (int)(frame_numerator / frame_denominator))
        frame = (int)(frame_numerator / frame_denominator) - 1;
    
    if (drop_frame) {
        int d = 2; // always 2 frames dropped per minute
        if ((tc->mins % 10) != 0 && frame < d) {
            frame = d;
        }
    }
    tc->frame = frame;
}

// Get the current timecode without buffer compensation for display
void get_display_timecode(SMPTETimecode *tc, double fps, int drop_frame, int64_t ntp_offset) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    
    // Convert to microseconds (64-bit integer)
    int64_t time_us = (int64_t)ts.tv_sec * MICROSECONDS_PER_SECOND + 
                    (int64_t)(ts.tv_nsec / NANOSECONDS_PER_MICROSECOND);
    
    // Apply NTP offset if needed, but don't add the buffer delay
    time_us += ntp_offset;
    
    // Convert back to seconds and fraction for localtime
    time_t adj_whole = (time_t)(time_us / MICROSECONDS_PER_SECOND);
    int64_t adj_frac_us = time_us % MICROSECONDS_PER_SECOND;
    
    struct tm *tm = localtime(&adj_whole);
    
    tc->years   = tm->tm_year + 1900;
    tc->months  = tm->tm_mon + 1;
    tc->days    = tm->tm_mday;
    tc->hours   = tm->tm_hour;
    tc->mins    = tm->tm_min;
    tc->secs    = tm->tm_sec;
    
    // Calculate frame using precise frame boundaries
    int64_t frame_numerator, frame_denominator;
    
    if (fps == 29.97) {
        frame_numerator = 30000;
        frame_denominator = 1001;
    } else if (fps == 23.976) {
        frame_numerator = 24000;
        frame_denominator = 1001;
    } else {
        frame_numerator = (int64_t)(fps * 1000);
        frame_denominator = 1000;
    }
    
    int64_t us_per_frame = (MICROSECONDS_PER_SECOND * frame_denominator) / frame_numerator;
    int frame = (int)(adj_frac_us / us_per_frame);
    
    if (frame >= (int)(frame_numerator / frame_denominator))
        frame = (int)(frame_numerator / frame_denominator) - 1;
    
    if (drop_frame) {
        int d = 2; // always 2 frames dropped per minute
        if ((tc->mins % 10) != 0 && frame < d) {
            frame = d;
        }
    }
    tc->frame = frame;
}

// Find framerate_spec_t from arg, or NULL if not found
const framerate_spec_t* parse_rate(const char* arg) {
    for (size_t i = 0; i < NUM_SUPPORTED_RATES; ++i) {
        if (strcmp(arg, supported_rates[i].name) == 0)
            return &supported_rates[i];
    }
    return NULL;
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
    SMPTETimecode tc, last_tc = {0};
    
    while (display->running) {
        // Get current NTP offset (safe to read without mutex as it's atomic)
        int64_t current_ntp_offset = 0;
        if (use_ntp) {
            pthread_mutex_lock(&ntp_lock);
            current_ntp_offset = ntp_offset_us;
            pthread_mutex_unlock(&ntp_lock);
        }
        
        // Generate the display timecode in the display thread
        get_display_timecode(&tc, display->fps, display->drop_frame, current_ntp_offset);
        
        // Only update the display if the timecode changed
        if (memcmp(&tc, &last_tc, sizeof(SMPTETimecode)) != 0) {
            format_timecode(buf, sizeof(buf), &tc, display->fps, display->drop_frame);
            fwrite(buf, 1, strlen(buf), stdout);
            fflush(stdout);
            last_tc = tc;
        }
        
        usleep(5000); // 5ms is plenty responsive for console display
    }
    printf("\n");
    return NULL;
}

void set_realtime_priority(void) {
    struct sched_param sp;
    sp.sched_priority = 20; // 1-99 (99=highest), 20 is safe for non-root, adjust as needed
    
    // First try SCHED_FIFO (strict real-time)
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) {
        return; // Success
    }
    
    // If FIFO fails, try SCHED_RR (round-robin real-time)
    if (pthread_setschedparam(pthread_self(), SCHED_RR, &sp) == 0) {
        fprintf(stderr, "Note: Using round-robin scheduler instead of FIFO\n");
        return; // Success with fallback
    }
    
    fprintf(stderr, "Warning: Failed to set real-time priority for audio thread: %s\n", 
            strerror(errno));
            
    // Try to at least elevate the nice value as a last resort
    if (nice(-20) == -1) {
        fprintf(stderr, "Warning: Failed to set process nice value: %s\n", 
                strerror(errno));
    }
}

// Return 1 if attached to a terminal, 0 otherwise
int is_console_interactive(void) {
    // Only consider interactive if stdout is a tty and not running under systemd
    return isatty(STDOUT_FILENO) && (getenv("INVOCATION_ID") == NULL);
}

// Configure ALSA PCM for minimal latency while maintaining stability
int configure_alsa_for_low_latency(snd_pcm_t *pcm, unsigned int rate, int ltc_frame_size) {
    int err;
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_sw_params_t *sw_params;
    
    // Allocate parameter structures
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_sw_params_alloca(&sw_params);
    
    // Initialize hardware parameters with full configuration space
    if ((err = snd_pcm_hw_params_any(pcm, hw_params)) < 0) {
        fprintf(stderr, "Cannot initialize hardware parameters: %s\n", snd_strerror(err));
        return err;
    }
    
    // Set access type
    if ((err = snd_pcm_hw_params_set_access(pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        fprintf(stderr, "Cannot set access type: %s\n", snd_strerror(err));
        return err;
    }
    
    // Set sample format
    if ((err = snd_pcm_hw_params_set_format(pcm, hw_params, SND_PCM_FORMAT_S16_LE)) < 0) {
        fprintf(stderr, "Cannot set sample format: %s\n", snd_strerror(err));
        return err;
    }
    
    // Set sample rate
    unsigned int exact_rate = rate;
    if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw_params, &exact_rate, 0)) < 0) {
        fprintf(stderr, "Cannot set sample rate: %s\n", snd_strerror(err));
        return err;
    }
    if (exact_rate != rate) {
        fprintf(stderr, "Warning: Sample rate adjusted from %u to %u Hz\n", rate, exact_rate);
    }
    
    // Set number of channels
    if ((err = snd_pcm_hw_params_set_channels(pcm, hw_params, CHANNELS)) < 0) {
        fprintf(stderr, "Cannot set channel count: %s\n", snd_strerror(err));
        return err;
    }
    
    // Calculate buffer size based on LTC frame size
    // Aim for reasonable buffer that can hold multiple frames but still has low latency
    snd_pcm_uframes_t buffer_size = ltc_frame_size * 4; // 4 frames worth of buffer
    if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw_params, &buffer_size)) < 0) {
        fprintf(stderr, "Cannot set buffer size: %s\n", snd_strerror(err));
        return err;
    }
    
    // Set period size to match LTC frame size for accurate timing
    snd_pcm_uframes_t period_size = ltc_frame_size;
    int dir = 0;
    if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw_params, &period_size, &dir)) < 0) {
        fprintf(stderr, "Cannot set period size: %s\n", snd_strerror(err));
        return err;
    }
    
    // Commit hardware parameters
    if ((err = snd_pcm_hw_params(pcm, hw_params)) < 0) {
        fprintf(stderr, "Cannot set hardware parameters: %s\n", snd_strerror(err));
        return err;
    }
    
    // Configure software parameters for low latency
    if ((err = snd_pcm_sw_params_current(pcm, sw_params)) < 0) {
        fprintf(stderr, "Cannot get current software parameters: %s\n", snd_strerror(err));
        return err;
    }
    
    // Start transfers when the first period is filled
    if ((err = snd_pcm_sw_params_set_start_threshold(pcm, sw_params, period_size)) < 0) {
        fprintf(stderr, "Cannot set start threshold: %s\n", snd_strerror(err));
        return err;
    }
    
    // Allow transfer when at least one sample can be processed
    if ((err = snd_pcm_sw_params_set_avail_min(pcm, sw_params, 1)) < 0) {
        fprintf(stderr, "Cannot set minimum available frames: %s\n", snd_strerror(err));
        return err;
    }
    
    // Commit software parameters
    if ((err = snd_pcm_sw_params(pcm, sw_params)) < 0) {
        fprintf(stderr, "Cannot set software parameters: %s\n", snd_strerror(err));
        return err;
    }
    
    // Pre-allocate events for snd_pcm_wait
    if ((err = snd_pcm_prepare(pcm)) < 0) {
        fprintf(stderr, "Cannot prepare audio interface: %s\n", snd_strerror(err));
        return err;
    }
    
    // Print actual buffer and period size for debugging
    snd_pcm_uframes_t actual_buffer_size;
    snd_pcm_uframes_t actual_period_size;
    
    snd_pcm_hw_params_get_buffer_size(hw_params, &actual_buffer_size);
    snd_pcm_hw_params_get_period_size(hw_params, &actual_period_size, &dir);
    
    fprintf(stderr, "ALSA buffer configuration: period_size=%lu, buffer_size=%lu (%.2f ms latency)\n",
            actual_period_size, actual_buffer_size, 
            (float)actual_buffer_size * 1000.0f / (float)rate);
            
    // Try to disable ALSA's internal resampling if possible
    if ((err = snd_pcm_hw_params_set_rate_resample(pcm, hw_params, 0)) == 0) {
        fprintf(stderr, "Disabled ALSA resampling for lower latency\n");
    }
    
    return 0;
}