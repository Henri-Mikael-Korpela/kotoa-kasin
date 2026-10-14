/**
 * @file ip.h
 * @brief IPv4 and IPv6 client addresses.
 *
 * Both kinds are kept as 16 bytes: an IPv4 address as an IPv4-mapped IPv6
 * address (::ffff:a.b.c.d), so they compare and hash the same way.
 */

#ifndef __KASIN_IP_H__
#define __KASIN_IP_H__

#include <stdbool.h>
#include <stdint.h>

#include "str.h"

#define IP_TEXT_SIZE 46  /**< Longest text form (INET6_ADDRSTRLEN), with its terminator. */

typedef struct {
    uint8_t bytes[16];
} Ip_Address;

/**
 * Makes an address from IPv4 bytes in network order.
 */
Ip_Address ip_from_v4(uint8_t const bytes[4]);

/**
 * Reads an address in text form: dotted IPv4 or IPv6.
 *
 * @return False when the text is not an address.
 */
bool ip_parse(Str text, Ip_Address *out);

/**
 * Whether an address is an IPv4 address (IPv4-mapped).
 */
bool ip_is_v4(Ip_Address const *ip);

/**
 * Appends an address in text form; IPv4 addresses as a.b.c.d.
 */
void ip_format(Ip_Address const *ip, Str_Builder *out);

bool ip_eq(Ip_Address const *a, Ip_Address const *b);

/**
 * The address of the client as seen by the nearest proxy: the last entry of an
 * X-Forwarded-For header. Each proxy appends the address it received the
 * request from, so earlier entries are whatever the client claimed and the
 * last is the only one a trusted proxy vouches for.
 *
 * @return False when the header is empty or its last entry is not an address.
 */
bool ip_from_forwarded_for(Str header, Ip_Address *out);

#endif  // __KASIN_IP_H__
