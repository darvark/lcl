#include "maidenhead.h"

#include <math.h>

#define DEG2RAD(x) ((x) * M_PI / 180.0)
#define RAD2DEG(x) ((x) * 180.0 / M_PI)

/*
 * Convert a Maidenhead locator to latitude and longitude.
 *
 * @param locator Maidenhead grid locator string.
 * @param lat Output latitude in degrees.
 * @param lon Output longitude in degrees.
 * @return 0 on success, or -1 on invalid input.
 */
int locator_to_latlon(const char *locator, double *lat, double *lon) {
  if (!locator || !lat || !lon)
    return -1;

  size_t len = strlen(locator);

  if (len < 4)
    return -1;

  char a = toupper(locator[0]);
  char b = toupper(locator[1]);

  char c = locator[2];
  char d = locator[3];

  if (a < 'A' || a > 'R')
    return -1;

  if (b < 'A' || b > 'R')
    return -1;

  if (!isdigit((unsigned char)c))
    return -1;

  if (!isdigit((unsigned char)d))
    return -1;

  double longitude = (a - 'A') * 20.0 - 180.0;

  double latitude = (b - 'A') * 10.0 - 90.0;

  longitude += (c - '0') * 2.0;
  latitude += (d - '0');

  if (len >= 6) {
    char e = tolower(locator[4]);
    char f = tolower(locator[5]);

    if (e >= 'a' && e <= 'x')
      longitude += (e - 'a') / 12.0;

    if (f >= 'a' && f <= 'x')
      latitude += (f - 'a') / 24.0;

    longitude += 1.0 / 24.0;
    latitude += 1.0 / 48.0;
  } else {
    longitude += 1.0;
    latitude += 0.5;
  }

  *lat = latitude;
  *lon = longitude;

  return 0;
}
