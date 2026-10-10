#ifndef CONTEST_RULES_H
#define CONTEST_RULES_H

#include "contest.h"

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

int contest_rules_validate_target_qso(const ContestDefinition *definition,
                                      const char *contest_name, time_t utc_time,
                                      int frequency_khz, const char *mode,
                                      int power_watts, char *error_text,
                                      size_t error_size);
int contest_definition_station_in_region(const ContestDefinition *definition,
                                                                                const char *region_name,
                                                                                const char *station_call,
                                                                                const char *station_exchange);
time_t contest_rules_now_utc(void);

#ifdef LOGGER_TESTING
void contest_rules_set_test_time(time_t utc_time);
#endif

#ifdef __cplusplus
}
#endif

#endif