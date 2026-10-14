/**
 * @file geoip.h
 * @brief The country of an IP address, looked up offline.
 *
 * The data is DB-IP's free "IP to Country Lite" database (CC BY 4.0, which
 * asks for the attribution "IP Geolocation by DB-IP" wherever the countries
 * are shown). kasin-geoip turns its CSV into a compact file (geoip_build.h),
 * which the server maps into memory read-only and searches; no address ever
 * leaves the server.
 *
 * The file holds, for IPv4 and for IPv6, the sorted start addresses of ranges
 * that cover every address, each with an ISO 3166-1 alpha-2 country code ("ZZ"
 * for reserved and unknown ranges):
 *
 *     "KGEOIP1\n"
 *     u32 v4_count, u32 v6_count            (little-endian)
 *     v4_count x 4 bytes   IPv4 range starts (network order)
 *     v4_count x 2 bytes   their country codes
 *     v6_count x 16 bytes  IPv6 range starts (network order)
 *     v6_count x 2 bytes   their country codes
 *
 * An address belongs to the last range that starts at or before it.
 */

#ifndef __KASIN_GEOIP_H__
#define __KASIN_GEOIP_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ip.h"

#define GEOIP_MAGIC "KGEOIP1\n"
#define GEOIP_MAGIC_SIZE 8
#define GEOIP_HEADER_SIZE (GEOIP_MAGIC_SIZE + 8)

typedef struct {
    uint8_t const *map;  /**< The mapped file, or NULL when no data is loaded. */
    size_t         map_size;
    uint32_t       v4_count;
    uint32_t       v6_count;
    uint8_t const *v4_starts;
    uint8_t const *v4_countries;
    uint8_t const *v6_starts;
    uint8_t const *v6_countries;
} Geoip;

/**
 * Maps a file made by kasin-geoip.
 *
 * @return False, with a message on stderr, when it cannot be read or is not
 *         such a file; g is then empty and every lookup says unknown.
 */
bool geoip_open(Geoip *g, char const *path);

void geoip_close(Geoip *g);

/**
 * Finds the country of an address.
 *
 * @param country Receives two uppercase letters, like "FI".
 * @return False when the country is unknown: no data, or a reserved or
 *         private range.
 */
bool geoip_country(Geoip const *g, Ip_Address const *ip, char country[2]);

#endif  // __KASIN_GEOIP_H__
