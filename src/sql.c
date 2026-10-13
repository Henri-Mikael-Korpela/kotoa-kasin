#include "sql.h"

#include <stdio.h>

bool sql_exec(sqlite3 *conn, Str sql, Str context) {
    char const *rest = sql.data;
    char const *end = sql.data + sql.len;
    while (rest < end) {
        sqlite3_stmt *stmt = NULL;
        char const *tail = NULL;
        if (sqlite3_prepare_v2(conn, rest, (int)(end - rest), &stmt, &tail) != SQLITE_OK) {
            fprintf(stderr, "db: %.*s: %s\n", STR_ARG(context), sqlite3_errmsg(conn));
            return false;
        }
        rest = tail;
        if (!stmt) {
            continue;  // Only whitespace or a comment.
        }
        int rc;
        do {
            rc = sqlite3_step(stmt);
        } while (rc == SQLITE_ROW);
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "db: %.*s: %s\n", STR_ARG(context), sqlite3_errmsg(conn));
            sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_finalize(stmt);
    }
    return true;
}

bool sql_prepare(sqlite3 *conn, sqlite3_stmt **stmt, Str sql, bool persistent) {
    unsigned int flags = persistent ? SQLITE_PREPARE_PERSISTENT : 0;
    if (sqlite3_prepare_v3(conn, sql.data, (int)sql.len, flags, stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "db: cannot prepare statement: %s\n", sqlite3_errmsg(conn));
        return false;
    }
    return true;
}

bool sql_bind_str(sqlite3_stmt *stmt, int index, Str value) {
    // An empty Str may carry a NULL pointer, which SQLite would read as NULL.
    char const *data = value.len > 0 ? value.data : "";
    return sqlite3_bind_text(stmt, index, data, (int)value.len, SQLITE_TRANSIENT) == SQLITE_OK;
}

Str sql_column_str(sqlite3_stmt *stmt, int column) {
    char const *text = (char const *)sqlite3_column_text(stmt, column);
    return str_make(text ? text : "", (size_t)sqlite3_column_bytes(stmt, column));
}
