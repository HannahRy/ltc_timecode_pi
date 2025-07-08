#include "ltc_config.h"
#include "ltc_ntp.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// Global variables
char config_device[128] = "";
char config_framerate[32] = "";

void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [-q] [-d device] [--config <file>] [--ntp-server <host>] [--ntp-sync-interval <seconds>] [frame_rate]\n", prog);
    fprintf(stderr, "  -q, --quiet                   Suppress console timecode output (recommended for service)\n");
    fprintf(stderr, "  -d, --device                  ALSA PCM device string (default: \"default\")\n");
    fprintf(stderr, "  --config <file>               Use specified config file (default: /etc/ltc_timecode_pi.conf)\n");
    fprintf(stderr, "  --ntp-server <host>           Sync to NTP server instead of system clock\n");
    fprintf(stderr, "  --ntp-sync-interval <seconds> Set NTP sync interval in seconds (default: 60)\n");
    fprintf(stderr, "  --ntp-slew-period <seconds>   Period over which to gradually adjust time (default: 30)\n");
    fprintf(stderr, "Supported frame rates:\n");
    for (size_t i = 0; i < NUM_SUPPORTED_RATES; ++i) {
        fprintf(stderr, "  %s\n", supported_rates[i].name);
    }
}

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
        } else if (strcmp(key, "ntp-server") == 0) {
            strncpy(ntp_server, val, sizeof(ntp_server)-1);
            use_ntp = 1;
        } else if (strcmp(key, "ntp-sync-interval") == 0) {
            ntp_sync_interval = atoi(val);
            if (ntp_sync_interval < 1) {
                ntp_sync_interval = 60; // Default to 1 minute if invalid
            }
        } else if (strcmp(key, "ntp-slew-period") == 0) {
            ntp_slew_period = atoi(val);
            if (ntp_slew_period < 1) {
                ntp_slew_period = 30; // Default to 30 seconds if invalid
            }
        }
    }
    
    fclose(f);
}

// Helper function to get config value by key from the global config file
int get_config_value(const char *key, char *value, size_t value_size) {
    if (!key || !value || value_size == 0) {
        return 0;
    }
    
    FILE *f = fopen(DEFAULT_CONFIG_FILE, "r");
    if (!f) {
        return 0;
    }
    
    char line[MAX_LINE];
    int found = 0;
    
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line;
        char *v = eq + 1;
        
        // Remove trailing newline
        v[strcspn(v, "\r\n")] = 0;
        
        // Trim leading/trailing whitespace from key
        while (*k && isspace(*k)) k++;
        char *end = k + strlen(k) - 1;
        while (end > k && isspace(*end)) *end-- = 0;
        
        if (strcmp(k, key) == 0) {
            strncpy(value, v, value_size);
            value[value_size - 1] = 0;  // Ensure null termination
            found = 1;
            break;
        }
    }
    
    fclose(f);
    return found;
}