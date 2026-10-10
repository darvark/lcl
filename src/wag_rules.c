#define _POSIX_C_SOURCE 200809L

#include "wag_rules.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static int frequency_is_on_wag_band(int frequency_khz) {
  return (frequency_khz >= 3500 && frequency_khz <= 4000) ||
         (frequency_khz >= 7000 && frequency_khz <= 7300) ||
         (frequency_khz >= 14000 && frequency_khz <= 14350) ||
         (frequency_khz >= 21000 && frequency_khz <= 21450) ||
         (frequency_khz >= 28000 && frequency_khz <= 29700);
}

static int frequency_is_contest_free(int frequency_khz, const char *mode) {
  const int cw = mode && strcmp(mode, "CW") == 0;
  const int ssb = mode && strcmp(mode, "SSB") == 0;

  if (cw) {
    return (frequency_khz >= 3560 && frequency_khz <= 3800) ||
           (frequency_khz >= 7040 && frequency_khz <= 7200) ||
           (frequency_khz >= 14060 && frequency_khz <= 14350);
  }
  if (!ssb)
    return 0;

  return (frequency_khz >= 3650 && frequency_khz <= 3700) ||
         (frequency_khz >= 7080 && frequency_khz <= 7130) ||
         (frequency_khz >= 14100 && frequency_khz <= 14125) ||
         (frequency_khz >= 14280 && frequency_khz <= 14350) ||
         (frequency_khz >= 21350 && frequency_khz <= 21450) ||
         (frequency_khz >= 28225 && frequency_khz <= 28400);
}

static int power_matches_category(int power_watts, const char *category) {
  if (power_watts <= 0 || !category)
    return 0;
  if (strcasecmp(category, "QRP") == 0)
    return power_watts <= 5;
  if (strcasecmp(category, "LOW") == 0)
    return power_watts <= 100;
  if (strcasecmp(category, "HIGH") == 0)
    return power_watts > 100;
  return 0;
}

static int reject(char *error_text, size_t error_size, const char *message) {
  if (error_text && error_size > 0)
    snprintf(error_text, error_size, "%s", message);
  return 0;
}

int wag_rules_validate_qso(time_t utc_time, int frequency_khz,
                           const char *mode, int power_watts,
                           const char *power_category,
                           char *error_text, size_t error_size) {
  if (error_text && error_size > 0)
    error_text[0] = 0;
  (void)utc_time;

  if (!mode || (strcmp(mode, "CW") != 0 && strcmp(mode, "SSB") != 0))
    return reject(error_text, error_size, "WAG allows CW and SSB only");

  if (!frequency_is_on_wag_band(frequency_khz))
    return reject(error_text, error_size, "Frequency is outside WAG bands");

  if (frequency_is_contest_free(frequency_khz, mode))
    return reject(error_text, error_size, "Frequency is in a WAG contest-free segment");

  if (power_watts <= 0)
    return reject(error_text, error_size, "Set STATION_TX_POWER_WATTS for WAG");

  if (!power_matches_category(power_watts, power_category))
    return reject(error_text, error_size, "TX power does not match WAG category");

  return 1;
}

#ifdef LOGGER_TESTING
static time_t test_time_utc = (time_t)-1;

void wag_rules_set_test_time(time_t utc_time) { test_time_utc = utc_time; }
#endif

time_t wag_rules_now_utc(void) {
#ifdef LOGGER_TESTING
  if (test_time_utc != (time_t)-1)
    return test_time_utc;
#endif
  return time(NULL);
}