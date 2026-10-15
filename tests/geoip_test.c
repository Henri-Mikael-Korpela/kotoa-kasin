/**
 * @file geoip_test.c
 * @brief Tests for client addresses and the country file.
 *
 * Run with ctest, or directly: geoip-test. Exits with EXIT_FAILURE when a test fails.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/geoip.h"
#include "../src/geoip_build.h"

static int FAILURES = 0;

static void check(char const *label, bool ok) {
    if (!ok) {
        printf("FAIL %s\n", label);
        FAILURES++;
    }
}

static void expect_text(char const *label, Ip_Address const *ip, char const *expected) {
    char buf[IP_TEXT_SIZE];
    Str_Builder text;
    str_builder_init(&text, buf, sizeof buf);
    ip_format(ip, &text);
    if (!str_eq(str_builder_str(&text), str_from_cstr(expected))) {
        printf("FAIL %s: got %.*s, want %s\n", label, STR_ARG(str_builder_str(&text)), expected);
        FAILURES++;
    }
}

static void expect_country(Geoip const *g, char const *address, char const *expected) {
    Ip_Address ip;
    char country[2];
    if (!ip_parse(str_from_cstr(address), &ip)) {
        printf("FAIL cannot parse %s\n", address);
        FAILURES++;
        return;
    }
    bool const found = geoip_country(g, &ip, country);
    bool const ok = expected == NULL ? !found : found && memcmp(country, expected, 2) == 0;
    if (!ok) {
        printf("FAIL country of %s: got %.*s, want %s\n", address, found ? 2 : 7,
               found ? country : "unknown", expected == NULL ? "unknown" : expected);
        FAILURES++;
    }
}

static bool write_text(char const *path, char const *text) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return false;
    }
    fputs(text, f);
    return fclose(f) == 0;
}

int main(void) {
    // Addresses.
    Ip_Address ip;
    check("parse IPv4", ip_parse(STR("203.0.113.7"), &ip) && ip_is_v4(&ip));
    expect_text("IPv4 written back", &ip, "203.0.113.7");
    check("parse IPv6", ip_parse(STR("2001:DB8::1"), &ip) && !ip_is_v4(&ip));
    expect_text("IPv6 written back", &ip, "2001:db8::1");
    check("IPv4-mapped IPv6 is IPv4", ip_parse(STR("::ffff:192.0.2.1"), &ip) && ip_is_v4(&ip));
    expect_text("IPv4-mapped written as IPv4", &ip, "192.0.2.1");
    check("not addresses", !ip_parse(STR(""), &ip) && !ip_parse(STR("1.2.3"), &ip) &&
                           !ip_parse(STR("1.2.3.4:80"), &ip) &&
                           !ip_parse(STR("example.com"), &ip) &&
                           !ip_parse(STR("1.2.3.4 "), &ip));

    // X-Forwarded-For: the last entry is the one the nearest proxy added.
    check("single entry", ip_from_forwarded_for(STR("198.51.100.20"), &ip));
    expect_text("single entry", &ip, "198.51.100.20");
    check("spoofed first entry ignored",
          ip_from_forwarded_for(STR("1.1.1.1, 10.0.0.1 ,198.51.100.20"), &ip));
    expect_text("last entry used", &ip, "198.51.100.20");
    check("IPv6 entry", ip_from_forwarded_for(STR("1.1.1.1, 2001:db8::7"), &ip));
    expect_text("IPv6 entry", &ip, "2001:db8::7");
    check("empty header", !ip_from_forwarded_for(STR(""), &ip));
    check("garbage last entry", !ip_from_forwarded_for(STR("198.51.100.20, nonsense"), &ip));

    // A small CSV in DB-IP's format, with a gap, ranges to merge and both families.
    char csv[] = "/tmp/geoip-test-XXXXXX";
    int const fd = mkstemp(csv);
    check("temporary file", fd >= 0);
    close(fd);
    char bin[sizeof csv + 4];
    snprintf(bin, sizeof bin, "%s.bin", csv);
    check("write CSV", write_text(csv,
        "0.0.0.0,0.255.255.255,ZZ\r\n"
        "1.0.0.0,1.0.0.255,AU\n"
        "1.0.1.0,1.0.3.255,CN\n"
        "1.0.4.0,1.0.7.255,CN\n"
        "5.0.0.0,5.0.0.255,FI\n"  // A gap before it: unknown.
        "::,1fff:ffff:ffff:ffff:ffff:ffff:ffff:ffff,ZZ\n"
        "2000::,2001:db7:ffff:ffff:ffff:ffff:ffff:ffff,SE\n"
        "2001:db8::,2001:db8:ffff:ffff:ffff:ffff:ffff:ffff,FI\n"));
    check("build", geoip_build_file(csv, bin));
    Geoip g;
    check("open", geoip_open(&g, bin));
    check("same-country ranges merged", g.v4_count == 6);  // ZZ AU CN ZZ(gap) FI ZZ(after)
    expect_country(&g, "0.1.2.3", NULL);
    expect_country(&g, "1.0.0.0", "AU");
    expect_country(&g, "1.0.0.255", "AU");
    expect_country(&g, "1.0.1.0", "CN");
    expect_country(&g, "1.0.6.9", "CN");
    expect_country(&g, "2.3.4.5", NULL);
    expect_country(&g, "5.0.0.128", "FI");
    expect_country(&g, "5.0.1.0", NULL);
    expect_country(&g, "255.255.255.255", NULL);
    expect_country(&g, "::1", NULL);
    expect_country(&g, "2000::1", "SE");
    expect_country(&g, "2001:db8:1::2", "FI");
    expect_country(&g, "2001:db9::", NULL);
    expect_country(&g, "ffff::", NULL);
    geoip_close(&g);
    check("closed lookups are unknown", !geoip_country(&g, &ip, (char[2]){0, 0}));

    // Malformed input is refused with the line.
    check("unsorted refused", write_text(csv, "1.0.1.0,1.0.3.255,CN\n1.0.0.0,1.0.0.255,AU\n") &&
                              !geoip_build_file(csv, bin));
    check("bad country refused",
          write_text(csv, "1.0.0.0,1.0.0.255,au\n") && !geoip_build_file(csv, bin));
    check("bad address refused",
          write_text(csv, "1.0.0.0,1.0.0.300,AU\n") && !geoip_build_file(csv, bin));
    check("not a country file", write_text(bin, "something else entirely") && !geoip_open(&g, bin));
    unlink(csv);
    unlink(bin);

    if (FAILURES > 0) {
        printf("%d failure%s\n", FAILURES, FAILURES == 1 ? "" : "s");
        return EXIT_FAILURE;
    }
    printf("all address and country tests passed\n");
    return EXIT_SUCCESS;
}
