# Timing Correction System Documentation

## Overview

This document explains the advanced timing correction mechanisms implemented in the LTC Timecode Generator to achieve precise frame timing across various framerates.

## Key Timing Challenges

Real-time audio timecode generation faces several timing-related challenges:

1. **System Latency**: Delays between the time a frame is calculated and when it's actually output
2. **Buffer Latency**: Audio hardware buffers introduce varying delays
3. **Second Boundary Jitter**: Particular timing difficulties at the start of each second
4. **Processing Overhead**: CPU scheduling and execution time variations

## Multi-Layered Timing Correction

The LTC Timecode Generator employs a multi-layered approach to timing correction:

### 1. Hardware-Level Optimizations

- **CPU Core Pinning**: Configurable via `cpu-core` setting (default: core 3)
- **Memory Locking**: Prevents memory paging using `mlockall()`
- **Real-time Priority**: Uses SCHED_FIFO or SCHED_RR with fallback mechanisms

### 2. ALSA Buffer Compensation

The system measures actual ALSA buffer delay in sample frames and compensates for it:

```c
delay_frames = snd_pcm_status_get_delay(status);
buffer_delay_us = (delay_frames * MICROSECONDS_PER_SECOND + (SAMPLE_RATE / 2)) / SAMPLE_RATE;
```

### 3. Non-Linear Adaptive Correction

The most sophisticated part of the timing system applies variable compensation depending on position within each second:

```c
// Calculate frame fraction within the current second (0.0 to 1.0)
double second_fraction = (double)(ts.tv_nsec) / 1000000000.0;

// Variable correction parameters
int64_t min_frames_offset = 1.0;  // Minimum frames offset (near end of second)
int64_t max_frames_offset = 3.5;  // Maximum frames offset (near start of second)
```

This addresses the observation that timing inaccuracies are generally higher at the start of each second.

### 4. Mathematical Correction Models

The system combines three mathematical approaches for timing correction:

#### Exponential Decay Model

```c
double decay_rate = 3.0;  // Higher value for faster transition to lower correction
double normalized_position = 1.0 - exp(-decay_rate * second_fraction);
double offset_frames = max_frames_offset - (normalized_position * (max_frames_offset - min_frames_offset));
```

This provides higher correction at the beginning of each second that rapidly decreases.

#### Sinusoidal Phase Adjustment

```c
double phase_adjustment = 0.2 * sin(2 * M_PI * second_fraction);
offset_frames += phase_adjustment;
```

This applies a gentle oscillatory correction to account for periodic timing variations.

#### Quadratic Supplemental Correction

```c
offset_frames += 0.3 * (1.0 - second_fraction * second_fraction);
```

This adds additional correction that is strongest at second boundaries and diminishes quadratically.

## NTP Synchronization

When enabled, the NTP synchronization system:

1. Makes multiple queries to an NTP server to determine time offset
2. Calculates the minimum offset to reduce network latency impact
3. Gradually adjusts time using the calculated FPS to avoid timecode jumps

```c
int64_t adjust_frames = (int64_t)(ntp_slew_period * selected_fps);
ntp_adjustment_step_us = diff / adjust_frames;
```

## Performance Considerations

- The timing system is optimized for real-time performance but may still exhibit small variations
- USB audio interfaces generally provide better timing stability than onboard audio
- CPU isolation (`isolcpus=3` kernel parameter) can further improve timing determinism
- For frame-critical applications, NTP synchronization with a local stratum 1 timeserver is recommended