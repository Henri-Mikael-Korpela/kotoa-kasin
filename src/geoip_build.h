/**
 * @file geoip_build.h
 * @brief Turns DB-IP's "IP to Country Lite" CSV into the file geoip.h reads.
 *
 * The CSV has one range per line, "first,last,CC", sorted, IPv4 and IPv6 in
 * any order. Ranges that follow each other with the same country are merged,
 * and any gap between ranges is filled with "ZZ" (unknown), so every address
 * falls in exactly one range and only the starts need to be stored.
 */

#ifndef __KASIN_GEOIP_BUILD_H__
#define __KASIN_GEOIP_BUILD_H__

#include <stdbool.h>

/**
 * Reads the CSV at csv_path and writes the country file to out_path (through
 * a temporary file, so a reader never sees half of it).
 *
 * @return False, with a message on stderr naming the line, when the CSV is not
 *         as expected or a file cannot be read or written.
 */
bool geoip_build_file(char const *csv_path, char const *out_path);

#endif  // __KASIN_GEOIP_BUILD_H__
