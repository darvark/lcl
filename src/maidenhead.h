#ifndef MAIDENHEAD_H
#define MAIDENHEAD_H

#include "globals.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Convert a Maidenhead locator to latitude and longitude.
 *
 * @param locator Maidenhead grid locator string.
 * @param lat Output latitude in degrees.
 * @param lon Output longitude in degrees.
 * @return 0 on success, or -1 on invalid input.
 */
int locator_to_latlon(const char *locator, double *lat, double *lon);

/*
 * Return non-zero when locator looks valid and can be parsed.
 */
int locator_is_valid(const char *locator);

/*
 * Calculate great-circle distance in kilometers between two locators.
 *
 * @return 0 on success, or -1 on invalid input.
 */
int locator_distance_km(const char *locator_a, const char *locator_b,
						int *distance_km);

#endif
