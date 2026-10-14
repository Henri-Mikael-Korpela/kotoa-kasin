#include "ip.h"

#include <arpa/inet.h>
#include <string.h>

// ::ffff:0:0/96, the prefix of IPv4-mapped addresses.
static uint8_t const V4_MAPPED_PREFIX[12] = {
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0xff,
    0xff,
};

Ip_Address ip_from_v4(uint8_t const bytes[4]) {
    Ip_Address ip;
    memcpy(ip.bytes, V4_MAPPED_PREFIX, sizeof V4_MAPPED_PREFIX);
    memcpy(ip.bytes + 12, bytes, 4);
    return ip;
}

bool ip_parse(Str text, Ip_Address *out) {
    char buf[IP_TEXT_SIZE];  // inet_pton() needs a NUL-terminated string.
    if (!str_to_cstr(text, buf, sizeof buf)) {
        return false;
    }
    uint8_t v4[4];
    if (inet_pton(AF_INET, buf, v4) == 1) {
        *out = ip_from_v4(v4);
        return true;
    }
    return inet_pton(AF_INET6, buf, out->bytes) == 1;
}

bool ip_is_v4(Ip_Address const *ip) {
    return memcmp(ip->bytes, V4_MAPPED_PREFIX, sizeof V4_MAPPED_PREFIX) == 0;
}

void ip_format(Ip_Address const *ip, Str_Builder *out) {
    char buf[IP_TEXT_SIZE];
    bool const v4 = ip_is_v4(ip);
    if (inet_ntop(v4 ? AF_INET : AF_INET6, v4 ? ip->bytes + 12 : ip->bytes, buf, sizeof buf) == NULL) {
        str_builder_append(out, STR("?"));
        return;
    }
    str_builder_append(out, str_from_cstr(buf));
}

bool ip_eq(Ip_Address const *a, Ip_Address const *b) {
    return memcmp(a->bytes, b->bytes, sizeof a->bytes) == 0;
}

bool ip_from_forwarded_for(Str header, Ip_Address *out) {
    size_t const comma = str_rfind_char(header, ',');
    Str const last = comma == STR_NPOS ? header : str_slice(header, comma + 1, header.len);
    return ip_parse(str_trim(last), out);
}
