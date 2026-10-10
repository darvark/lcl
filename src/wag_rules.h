#ifndef WAG_RULES_H
#define WAG_RULES_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

int wag_rules_validate_qso(time_t utc_time, int frequency_khz,
                           const char *mode, int power_watts,
                           const char *power_category,
                           char *error_text, size_t error_size);

time_t wag_rules_now_utc(void);

#ifdef LOGGER_TESTING
void wag_rules_set_test_time(time_t utc_time);
#endif

#ifdef __cplusplus
}
#endif

#endif