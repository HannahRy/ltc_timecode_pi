#ifndef LTC_CONFIG_H
#define LTC_CONFIG_H

#include "ltc_common.h"

// Configuration functions
void parse_config(const char *filename);
void print_usage(const char* prog);

// Helper function to get config value by key
int get_config_value(const char *key, char *value, size_t value_size);

// Global configuration variables
extern char config_device[128];
extern char config_framerate[32];

#endif // LTC_CONFIG_H