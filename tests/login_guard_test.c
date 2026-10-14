/**
 * @file login_guard_test.c
 * @brief Tests for the brute-force protection's rules.
 *
 * Run with ctest, or directly: login-guard-test. Exits with EXIT_FAILURE when a test fails.
 */

#include <stdio.h>
#include <stdlib.h>

#include "../src/login_guard.h"

static int FAILURES = 0;
static Login_Guard GUARD;  // Large: kept out of the stack.

static void check(char const *label, bool ok) {
    if (!ok) {
        printf("FAIL %s\n", label);
        FAILURES++;
    }
}

static Ip_Address address(char const *text) {
    Ip_Address ip;
    if (!ip_parse(str_from_cstr(text), &ip)) {
        printf("FAIL cannot parse %s\n", text);
        FAILURES++;
    }
    return ip;
}

// Fails n times in a row, each when allowed, starting at *now; returns the last decision.
static Login_Decision fail_times(Ip_Address const *ip, int n, int64_t *now) {
    Login_Decision d = {
        .verdict = LOGIN_ALLOWED,
    };
    for (int i = 0; i < n; i++) {
        Login_Decision const allowed = login_guard_check(&GUARD, ip, *now);
        if (allowed.verdict != LOGIN_ALLOWED) {
            *now += allowed.retry_after;
            i--;
            continue;
        }
        d = login_guard_failed(&GUARD, ip, *now);
    }
    return d;
}

int main(void) {
    // The waits.
    check("no wait for 3 failures", login_guard_wait(0) == 0 && login_guard_wait(3) == 0);
    check("2 s after the 4th", login_guard_wait(4) == 2);
    check("doubling", login_guard_wait(5) == 4 && login_guard_wait(6) == 8 && login_guard_wait(8) == 32);
    check("capped at 60 s", login_guard_wait(9) == 60);
    check("banned at 10", login_guard_wait(10) == LOGIN_BAN_SEC && login_guard_wait(500) == LOGIN_BAN_SEC);

    int64_t now = 1000000;
    login_guard_init(&GUARD, now);
    Ip_Address const attacker = address("203.0.113.7");
    Ip_Address const admin = address("198.51.100.20");
    Ip_Address const v6 = address("2001:db8::1");

    // Three free failures, then a wait that is enforced.
    Login_Decision d = fail_times(&attacker, 3, &now);
    check("3 failures: may try at once", d.verdict == LOGIN_ALLOWED && d.failures == 3);
    now += 60;  // Let the checks of the minute refill.
    d = fail_times(&attacker, 1, &now);
    check("4th failure: wait 2 s", d.verdict == LOGIN_DELAYED && d.retry_after == 2 && d.failures == 4);
    d = login_guard_check(&GUARD, &attacker, now + 1);
    check("refused within the wait", d.verdict == LOGIN_DELAYED && d.retry_after == 1);
    check("first refusal is logged", d.log_refusal && d.refused == 1);
    d = login_guard_check(&GUARD, &attacker, now + 1);
    check("second refusal within a minute is not logged", !d.log_refusal);
    check("other addresses are not affected", login_guard_check(&GUARD, &admin, now + 1).verdict == LOGIN_ALLOWED);
    d = login_guard_check(&GUARD, &attacker, now + 2);
    check("allowed after the wait", d.verdict == LOGIN_ALLOWED);

    // Banned after 10 failures, for LOGIN_BAN_SEC.
    now += 120;
    d = fail_times(&attacker, 6, &now);
    check("10th failure: banned", d.verdict == LOGIN_BANNED && d.failures == 10 && d.retry_after == LOGIN_BAN_SEC);
    int64_t const banned_at = now;
    d = login_guard_check(&GUARD, &attacker, banned_at + LOGIN_BAN_SEC - 1);
    check("still banned just before the end", d.verdict == LOGIN_BANNED && d.retry_after == 1);
    Login_Block blocks[4];
    size_t count = login_guard_blocks(&GUARD, banned_at + 10, blocks, 4);
    check("listed as banned", count == 1 && ip_eq(&blocks[0].ip, &attacker) && blocks[0].banned &&
                              blocks[0].until == banned_at + LOGIN_BAN_SEC && blocks[0].failures == 10);
    d = login_guard_check(&GUARD, &attacker, banned_at + LOGIN_BAN_SEC);
    check("ban over: allowed with failures forgotten", d.verdict == LOGIN_ALLOWED && d.failures == 0);
    count = login_guard_blocks(&GUARD, banned_at + LOGIN_BAN_SEC, blocks, 4);
    check("no longer listed", count == 0);

    // A success forgets the failures; an admin can lift a ban.
    now = banned_at + 2 * LOGIN_BAN_SEC;
    fail_times(&admin, 5, &now);
    login_guard_succeeded(&GUARD, &admin);
    d = login_guard_failed(&GUARD, &admin, now);
    check("success forgets failures", d.failures == 1 && d.verdict == LOGIN_ALLOWED);
    now += 60;
    d = fail_times(&v6, 10, &now);
    check("IPv6 banned too", d.verdict == LOGIN_BANNED);
    login_guard_clear(&GUARD, &v6);
    check("clear lifts the ban", login_guard_blocks(&GUARD, now, blocks, 4) == 0);

    // Failures restored from the database after a restart.
    login_guard_init(&GUARD, now);
    login_guard_restore(&GUARD, &attacker, 12, now - 100);
    d = login_guard_check(&GUARD, &attacker, now);
    check("restored ban", d.verdict == LOGIN_BANNED && d.retry_after == LOGIN_BAN_SEC - 100);
    login_guard_restore(&GUARD, &admin, 2, now - 5);
    check("restored few failures: allowed", login_guard_check(&GUARD, &admin, now).verdict == LOGIN_ALLOWED);

    // At most LOGIN_CHECKS_PER_MINUTE password checks a minute from everyone.
    login_guard_init(&GUARD, now);
    int allowed = 0;
    for (int i = 0; i < 20; i++) {
        uint8_t const bytes[4] = {
            192,
            0,
            2,
            (uint8_t)i,
        };
        Ip_Address const ip = ip_from_v4(bytes);
        d = login_guard_check(&GUARD, &ip, now);
        if (d.verdict == LOGIN_ALLOWED) {
            allowed++;
        }
    }
    check("only the minute's checks are allowed", allowed == LOGIN_CHECKS_PER_MINUTE);
    check("then busy, with a wait", d.verdict == LOGIN_BUSY && d.retry_after >= 1 && d.retry_after <= 60);
    d = login_guard_check(&GUARD, &admin, now + 60 / LOGIN_CHECKS_PER_MINUTE);
    check("one more check after its period", d.verdict == LOGIN_ALLOWED);
    d = login_guard_check(&GUARD, &admin, now + 60 / LOGIN_CHECKS_PER_MINUTE);
    check("but only one", d.verdict == LOGIN_BUSY);
    login_guard_init(&GUARD, now);
    fail_times(&attacker, 10, &now);
    uint32_t const tokens_before = GUARD.tokens;
    for (int i = 0; i < 50; i++) {
        login_guard_check(&GUARD, &attacker, now);
    }
    check("a banned address uses up no checks", GUARD.tokens == tokens_before);

    // A full table makes room by dropping the address whose last failure is oldest.
    login_guard_init(&GUARD, now);
    for (int i = 0; i < LOGIN_GUARD_SLOTS; i++) {
        uint8_t const bytes[4] = {
            10,
            (uint8_t)(i >> 16),
            (uint8_t)(i >> 8),
            (uint8_t)i,
        };
        Ip_Address const ip = ip_from_v4(bytes);
        login_guard_failed(&GUARD, &ip, i == 0 ? now : now + 1);  // 10.0.0.0 is the oldest.
    }
    d = login_guard_failed(&GUARD, &attacker, now + 2);
    check("a new address finds room when full", d.failures == 1);
    uint8_t const first[4] = {
        10,
        0,
        0,
        0,
    };
    Ip_Address const oldest = ip_from_v4(first);
    d = login_guard_failed(&GUARD, &oldest, now + 3);
    check("the oldest was the one dropped", d.failures == 1);
    uint8_t const later[4] = {
        10,
        0,
        0,
        5,
    };
    Ip_Address const kept = ip_from_v4(later);
    d = login_guard_failed(&GUARD, &kept, now + 4);
    check("others kept their failures", d.failures == 2);

    if (FAILURES > 0) {
        printf("%d failure%s\n", FAILURES, FAILURES == 1 ? "" : "s");
        return EXIT_FAILURE;
    }
    printf("all login guard tests passed\n");
    return EXIT_SUCCESS;
}
