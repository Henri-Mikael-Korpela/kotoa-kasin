/**
 * @file login_guard.h
 * @brief Brute-force protection for the login, shared by all worker threads.
 *
 * Failed logins are counted per client address:
 *
 *   - The first LOGIN_FREE_FAILURES failures cost nothing.
 *   - After that the address must wait before its next attempt: 2 seconds
 *     after the 4th failure, then 4, 8 and so on, at most LOGIN_MAX_DELAY_SEC.
 *   - After LOGIN_BAN_FAILURES failures it is banned for LOGIN_BAN_SEC.
 *   - Failures are forgotten LOGIN_WINDOW_SEC after the last one, or at once
 *     after a successful login.
 *
 * On top of that, at most LOGIN_CHECKS_PER_MINUTE passwords are checked per
 * minute in all, from every address together. A check is deliberately slow
 * (see password.h); this keeps attackers with many addresses from tying up the
 * processor. The address limits are applied first, so a banned address does
 * not use up checks.
 *
 * A refused attempt is answered at once, without checking any password.
 *
 * The state lives in a fixed table behind a mutex: no allocation, and logins
 * are rare enough that one lock is no bottleneck. When the table is full, the
 * address whose last failure is oldest makes room. Times are Unix seconds and
 * passed in, which keeps the rules testable.
 */

#ifndef __KASIN_LOGIN_GUARD_H__
#define __KASIN_LOGIN_GUARD_H__

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ip.h"

#define LOGIN_GUARD_SLOTS       4096
#define LOGIN_FREE_FAILURES     3
#define LOGIN_MAX_DELAY_SEC     60
#define LOGIN_BAN_FAILURES      10
#define LOGIN_BAN_SEC           (15 * 60)
#define LOGIN_WINDOW_SEC        (15 * 60)
#define LOGIN_CHECKS_PER_MINUTE 6
#define LOGIN_REFUSAL_LOG_SEC   60  /**< Refusals are logged at most this often per address. */

typedef enum {
    LOGIN_ALLOWED,
    LOGIN_DELAYED,  /**< The address must wait after its recent failures. */
    LOGIN_BANNED,   /**< The address failed LOGIN_BAN_FAILURES times. */
    LOGIN_BUSY,     /**< Too many passwords checked in the last minute, from all addresses. */
} Login_Verdict;

typedef struct {
    Login_Verdict verdict;
    int64_t       retry_after;  /**< Seconds until another attempt may be made; 0 when allowed. */
    uint32_t      failures;     /**< The address's recent failures. */
    bool          log_refusal;  /**< Whether this refusal should be logged (see LOGIN_REFUSAL_LOG_SEC). */
    uint32_t      refused;      /**< Refusals since the last logged one, this one included. */
} Login_Decision;

typedef struct {
    Ip_Address ip;
    int64_t    last_failure;  /**< 0 when the slot is free. */
    uint32_t   failures;
    uint32_t   refused;  /**< Refusals not logged yet. */
    int64_t    refusal_logged_at;
} Login_Guard_Entry;

typedef struct {
    pthread_mutex_t   lock;
    Login_Guard_Entry entries[LOGIN_GUARD_SLOTS];
    uint32_t          tokens;  /**< Password checks left in the current minute. */
    int64_t           tokens_at;  /**< When tokens was last topped up. */
    uint32_t          busy_refused;  /**< LOGIN_BUSY refusals not logged yet. */
    int64_t           busy_logged_at;
} Login_Guard;

/**
 * An address that must currently wait or is banned.
 */
typedef struct {
    Ip_Address ip;
    uint32_t   failures;
    int64_t    until;  /**< When it may try again. */
    bool       banned;
} Login_Block;

/**
 * The wait before the next attempt after a number of consecutive failures.
 */
int64_t login_guard_wait(uint32_t failures);

void login_guard_init(Login_Guard *g, int64_t now);

/**
 * Decides whether an address may try a password now. An allowed attempt uses
 * up one of the checks of the current minute.
 */
Login_Decision login_guard_check(Login_Guard *g, Ip_Address const *ip, int64_t now);

/**
 * Records a failed login.
 *
 * @return The address's state afterwards: its failures, and the wait before
 *         its next attempt (verdict LOGIN_DELAYED or LOGIN_BANNED), or
 *         LOGIN_ALLOWED when it may try again at once.
 */
Login_Decision login_guard_failed(Login_Guard *g, Ip_Address const *ip, int64_t now);

/**
 * Records a successful login: the address's failures are forgotten.
 */
void login_guard_succeeded(Login_Guard *g, Ip_Address const *ip);

/**
 * Restores an address's failures, read back from the database at startup, so
 * waits and bans survive a restart.
 */
void login_guard_restore(Login_Guard *g, Ip_Address const *ip, uint32_t failures, int64_t last_failure);

/**
 * Lifts an address's wait or ban (the admin's "unblock").
 */
void login_guard_clear(Login_Guard *g, Ip_Address const *ip);

/**
 * Lists the addresses that must currently wait or are banned.
 *
 * @return The number written to out, at most capacity.
 */
size_t login_guard_blocks(Login_Guard *g, int64_t now, Login_Block *out, size_t capacity);

#endif  // __KASIN_LOGIN_GUARD_H__
