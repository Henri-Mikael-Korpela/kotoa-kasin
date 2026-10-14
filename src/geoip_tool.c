/**
 * @file geoip_tool.c
 * @brief kasin-geoip: makes the server's country file from DB-IP's CSV.
 *
 * Usage: kasin-geoip <dbip-country-lite.csv> <out.bin>
 *
 * scripts/fetch-geoip downloads the CSV; see geoip.h for the file it makes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>

#include "geoip_build.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <dbip-country-lite.csv> <out.bin>\n", argv[0]);
        return EX_USAGE;
    }
    return geoip_build_file(argv[1], argv[2]) ? EXIT_SUCCESS : EX_DATAERR;
}
