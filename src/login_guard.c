#include "login_guard.h"

#include <string.h>

#define CHECK_PERIOD_SEC (60 / LOGIN_CHECKS_PER_MINUTE)  // One more check is allowed this often.

int64_t login_guard_wait(uint32_t failures) {
    if (failures >= LOGIN_BAN_FAILURES) {
        return LOGIN_BAN_SEC;
    }
    if (failures <= LOGIN_FREE_FAILURES) {
        return 0;
    }
    int64_t const wait = (int64_t)1 << (failures - LOGIN_FREE_FAILURES);
    return wait < LOGIN_MAX_DELAY_SEC ? wait : LOGIN_MAX_DELAY_SEC;
}

void login_guard_init(Login_Guard *g, int64_t now) {
    pthread_mutex_init(&g->lock, NULL);
    memset(g->entries, 0, sizeof g->entries);
    g->tokens = LOGIN_CHECKS_PER_MINUTE;
    g->tokens_at = now;
    g->busy_refused = 0;
    g->busy_logged_at = 0;
}

static bool is_stale(Login_Guard_Entry const *e, int64_t now) {
    return e->last_failure == 0 || now - e->last_failure >= LOGIN_WINDOW_SEC;
}

// The address's entry with recent failures, or NULL.
static Login_Guard_Entry *find(Login_Guard *g, Ip_Address const *ip, int64_t now) {
    for (size_t i = 0; i < LOGIN_GUARD_SLOTS; i++) {
        Login_Guard_Entry *e = &g->entries[i];
        if (e->last_failure != 0 && ip_eq(&e->ip, ip)) {
            return is_stale(e, now) ? NULL : e;
        }
    }
    return NULL;
}

/**
 * The address's entry, made if needed: from a free or stale slot, or else the
 * one whose last failure is oldest.
 */
static Login_Guard_Entry *find_or_add(Login_Guard *g, Ip_Address const *ip, int64_t now) {
    Login_Guard_Entry *free_slot = NULL;  // The first free or stale slot.
    Login_Guard_Entry *oldest = NULL;     // The live entry with the oldest last failure.
    for (size_t i = 0; i < LOGIN_GUARD_SLOTS; i++) {
        Login_Guard_Entry *e = &g->entries[i];
        if (e->last_failure != 0 && ip_eq(&e->ip, ip)) {
            if (is_stale(e, now)) {
                e->failures = 0;  // Its failures are forgotten.
            }
            return e;
        }
        if (is_stale(e, now)) {
            if (free_slot == NULL) {
                free_slot = e;
            }
        } else if (oldest == NULL || e->last_failure < oldest->last_failure) {
            oldest = e;
        }
    }
    Login_Guard_Entry *slot = free_slot != NULL ? free_slot : oldest;
    memset(slot, 0, sizeof *slot);
    slot->ip = *ip;
    return slot;
}

// The wait the entry's failures impose, as a decision.
static Login_Decision decision_for(Login_Guard_Entry const *e, int64_t now) {
    int64_t const until = e->last_failure + login_guard_wait(e->failures);
    Login_Decision d = {
        .verdict = LOGIN_ALLOWED,
        .retry_after = 0,
        .failures = e->failures,
        .log_refusal = false,
        .refused = 0,
    };
    if (now < until) {
        d.verdict = e->failures >= LOGIN_BAN_FAILURES ? LOGIN_BANNED : LOGIN_DELAYED;
        d.retry_after = until - now;
    }
    return d;
}

// Tops up the checks of the minute: one more every CHECK_PERIOD_SEC.
static void refill(Login_Guard *g, int64_t now) {
    if (now < g->tokens_at) {
        g->tokens_at = now;  // The clock went back.
        return;
    }
    int64_t const periods = (now - g->tokens_at) / CHECK_PERIOD_SEC;
    if (periods == 0) {
        return;
    }
    int64_t const tokens = (int64_t)g->tokens + periods;
    g->tokens = tokens >= LOGIN_CHECKS_PER_MINUTE ? LOGIN_CHECKS_PER_MINUTE : (uint32_t)tokens;
    g->tokens_at = g->tokens == LOGIN_CHECKS_PER_MINUTE ? now : g->tokens_at + periods * CHECK_PERIOD_SEC;
}

// Counts a refusal and says whether this one is to be logged.
static void count_refusal(uint32_t *refused, int64_t *logged_at, int64_t now, Login_Decision *d) {
    (*refused)++;
    if (now - *logged_at >= LOGIN_REFUSAL_LOG_SEC) {
        d->log_refusal = true;
        d->refused = *refused;
        *refused = 0;
        *logged_at = now;
    }
}

Login_Decision login_guard_check(Login_Guard *g, Ip_Address const *ip, int64_t now) {
    pthread_mutex_lock(&g->lock);
    Login_Decision d = {
        .verdict = LOGIN_ALLOWED,
        .retry_after = 0,
        .failures = 0,
        .log_refusal = false,
        .refused = 0,
    };
    Login_Guard_Entry *e = find(g, ip, now);
    if (e != NULL) {
        d = decision_for(e, now);
        if (d.verdict != LOGIN_ALLOWED) {
            count_refusal(&e->refused, &e->refusal_logged_at, now, &d);
            pthread_mutex_unlock(&g->lock);
            return d;
        }
    }
    refill(g, now);
    if (g->tokens == 0) {
        d.verdict = LOGIN_BUSY;
        d.retry_after = CHECK_PERIOD_SEC - (now - g->tokens_at);
        if (d.retry_after < 1) {
            d.retry_after = 1;
        }
        count_refusal(&g->busy_refused, &g->busy_logged_at, now, &d);
    } else {
        g->tokens--;
    }
    pthread_mutex_unlock(&g->lock);
    return d;
}

Login_Decision login_guard_failed(Login_Guard *g, Ip_Address const *ip, int64_t now) {
    pthread_mutex_lock(&g->lock);
    Login_Guard_Entry *e = find_or_add(g, ip, now);
    if (e->failures < UINT32_MAX) {
        e->failures++;
    }
    e->last_failure = now;
    Login_Decision const d = decision_for(e, now);
    pthread_mutex_unlock(&g->lock);
    return d;
}

void login_guard_succeeded(Login_Guard *g, Ip_Address const *ip) {
    login_guard_clear(g, ip);
}

void login_guard_restore(Login_Guard *g, Ip_Address const *ip, uint32_t failures, int64_t last_failure) {
    if (failures == 0 || last_failure <= 0) {
        return;
    }
    pthread_mutex_lock(&g->lock);
    Login_Guard_Entry *e = find_or_add(g, ip, last_failure);
    if (failures > e->failures) {
        e->failures = failures;
    }
    if (last_failure > e->last_failure) {
        e->last_failure = last_failure;
    }
    pthread_mutex_unlock(&g->lock);
}

void login_guard_clear(Login_Guard *g, Ip_Address const *ip) {
    pthread_mutex_lock(&g->lock);
    for (size_t i = 0; i < LOGIN_GUARD_SLOTS; i++) {
        Login_Guard_Entry *e = &g->entries[i];
        if (e->last_failure != 0 && ip_eq(&e->ip, ip)) {
            memset(e, 0, sizeof *e);
        }
    }
    pthread_mutex_unlock(&g->lock);
}

size_t login_guard_blocks(Login_Guard *g, int64_t now, Login_Block *out, size_t capacity) {
    size_t count = 0;
    pthread_mutex_lock(&g->lock);
    for (size_t i = 0; i < LOGIN_GUARD_SLOTS && count < capacity; i++) {
        Login_Guard_Entry const *e = &g->entries[i];
        if (is_stale(e, now)) {
            continue;
        }
        Login_Decision const d = decision_for(e, now);
        if (d.verdict == LOGIN_ALLOWED) {
            continue;
        }
        out[count++] = (Login_Block) {
            .ip = e->ip,
            .failures = e->failures,
            .until = now + d.retry_after,
            .banned = d.verdict == LOGIN_BANNED,
        };
    }
    pthread_mutex_unlock(&g->lock);
    return count;
}
