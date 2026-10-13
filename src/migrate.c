#include "migrate.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "str.h"

#define MAX_MIGRATIONS     256
#define MAX_FILE_NAME      128
#define MAX_PATH           1024
#define MAX_SQL_SIZE       (4 * 1024 * 1024)
#define VERSION_DIGITS     4

/*
 * Before migration files existed, the schema was compiled into the server and
 * PRAGMA user_version counted the steps. Version 2 is exactly what files 0001
 * and 0002 create, so such a database is adopted by recording those two as
 * applied without running them.
 */
#define LEGACY_SCHEMA_VERSION 2

typedef struct {
    int    version;
    char   file_name[MAX_FILE_NAME];  /**< For example "0001__create_users_table.sql". */
    size_t name_len;                  /**< Length of the name part ("create_users_table"). */
} Migration;

typedef enum {
    NAME_IGNORED,  /**< Not a .sql file; not a migration. */
    NAME_VALID,
    NAME_INVALID,  /**< A .sql file with a malformed name. */
} Name_Kind;

static Str migration_name(Migration const *m) {
    return str_make(m->file_name + VERSION_DIGITS + 2, m->name_len);
}

static Name_Kind classify_file_name(Str file_name, int *version, size_t *name_len) {
    Str suffix = STR(".sql");
    if (!str_ends_with(file_name, suffix)) {
        return NAME_IGNORED;
    }
    Str stem = str_slice(file_name, 0, file_name.len - suffix.len);
    Str number;
    Str name;
    uint64_t value;
    if (!str_split_once(stem, STR("__"), &number, &name) || number.len != VERSION_DIGITS ||
        !str_to_u64(number, &value) || value == 0 || name.len == 0) {
        return NAME_INVALID;
    }
    for (size_t i = 0; i < name.len; i++) {
        char c = name.data[i];
        bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!allowed) {
            return NAME_INVALID;
        }
    }
    *version = (int)value;
    *name_len = name.len;
    return NAME_VALID;
}

static int compare_versions(void const *a, void const *b) {
    int va = ((Migration const *)a)->version;
    int vb = ((Migration const *)b)->version;
    return (va > vb) - (va < vb);
}

static bool load_migrations(char const *dir, Migration *list, int *count) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "db: cannot open migrations directory '%s': %s\n", dir, strerror(errno));
        return false;
    }
    *count = 0;
    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir(d)) != NULL) {
        Str file_name = str_from_cstr(entry->d_name);
        int version;
        size_t name_len;
        Name_Kind kind = classify_file_name(file_name, &version, &name_len);
        if (kind == NAME_IGNORED) {
            continue;
        }
        if (kind == NAME_INVALID) {
            fprintf(stderr, "db: migration '%s' must be named like 0001__create_users_table.sql\n",
                    entry->d_name);
            ok = false;
        } else if (*count == MAX_MIGRATIONS || file_name.len >= MAX_FILE_NAME) {
            fprintf(stderr, "db: too many migrations, or a name too long, at '%s'\n", entry->d_name);
            ok = false;
        } else {
            Migration *m = &list[*count];
            m->version = version;
            m->name_len = name_len;
            str_to_cstr(file_name, m->file_name, sizeof m->file_name);
            (*count)++;
        }
    }
    closedir(d);
    if (!ok) {
        return false;
    }

    qsort(list, (size_t)*count, sizeof list[0], compare_versions);
    for (int i = 1; i < *count; i++) {
        if (list[i].version == list[i - 1].version) {
            fprintf(stderr, "db: migrations '%s' and '%s' share a number\n",
                    list[i - 1].file_name, list[i].file_name);
            return false;
        }
    }
    if (*count == 0) {
        fprintf(stderr, "db: no migrations found in '%s'\n", dir);
        return false;
    }
    return true;
}

/*
 * Reads a migration file into memory from SQLite's heap, NUL-terminated as
 * sqlite3_exec() wants it. Free with sqlite3_free().
 */
static char *read_sql(char const *dir, Migration const *m) {
    char path[MAX_PATH];
    int n = snprintf(path, sizeof path, "%s/%s", dir, m->file_name);
    if (n < 0 || (size_t)n >= sizeof path) {
        fprintf(stderr, "db: path to '%s' is too long\n", m->file_name);
        return NULL;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        fprintf(stderr, "db: cannot read '%s': %s\n", path, strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        return NULL;
    }
    if (st.st_size > MAX_SQL_SIZE) {
        fprintf(stderr, "db: '%s' is larger than %d bytes\n", path, MAX_SQL_SIZE);
        close(fd);
        return NULL;
    }

    size_t size = (size_t)st.st_size;
    char *sql = sqlite3_malloc64(size + 1);
    size_t got = 0;
    while (sql && got < size) {
        ssize_t r = read(fd, sql + got, size - got);
        if (r > 0) {
            got += (size_t)r;
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(fd);
    if (!sql || got != size) {
        fprintf(stderr, "db: cannot read '%s'\n", path);
        sqlite3_free(sql);
        return NULL;
    }
    sql[size] = '\0';
    return sql;
}

static bool exec(sqlite3 *conn, char const *sql, char const *context) {
    char *error = NULL;
    if (sqlite3_exec(conn, sql, NULL, NULL, &error) != SQLITE_OK) {
        fprintf(stderr, "db: %s: %s\n", context, error ? error : sqlite3_errmsg(conn));
        sqlite3_free(error);
        return false;
    }
    return true;
}

// Runs a query that returns one integer, such as a count or a version.
static bool query_int(sqlite3 *conn, char const *sql, int *value) {
    sqlite3_stmt *stmt = NULL;
    bool ok = sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) == SQLITE_OK &&
              sqlite3_step(stmt) == SQLITE_ROW;
    if (ok) {
        *value = sqlite3_column_int(stmt, 0);
    } else {
        fprintf(stderr, "db: %s: %s\n", sql, sqlite3_errmsg(conn));
    }
    sqlite3_finalize(stmt);
    return ok;
}

static bool is_applied(sqlite3 *conn, int version, bool *applied) {
    sqlite3_stmt *stmt = NULL;
    bool ok = sqlite3_prepare_v2(conn, "SELECT 1 FROM schema_migrations WHERE version = ?1",
                                 -1, &stmt, NULL) == SQLITE_OK;
    int rc = SQLITE_ERROR;
    if (ok) {
        sqlite3_bind_int(stmt, 1, version);
        rc = sqlite3_step(stmt);
        ok = rc == SQLITE_ROW || rc == SQLITE_DONE;
    }
    if (ok) {
        *applied = rc == SQLITE_ROW;
    } else {
        fprintf(stderr, "db: cannot read schema_migrations: %s\n", sqlite3_errmsg(conn));
    }
    sqlite3_finalize(stmt);
    return ok;
}

// An applied migration whose file was renamed or renumbered means history changed.
static bool check_applied_name(sqlite3 *conn, Migration const *m) {
    sqlite3_stmt *stmt = NULL;
    Str name = migration_name(m);
    bool ok = sqlite3_prepare_v2(conn, "SELECT name FROM schema_migrations WHERE version = ?1",
                                 -1, &stmt, NULL) == SQLITE_OK &&
              sqlite3_bind_int(stmt, 1, m->version) == SQLITE_OK &&
              sqlite3_step(stmt) == SQLITE_ROW;
    if (!ok) {
        fprintf(stderr, "db: cannot read schema_migrations: %s\n", sqlite3_errmsg(conn));
    } else {
        char const *stored = (char const *)sqlite3_column_text(stmt, 0);
        Str stored_name = str_make(stored ? stored : "", (size_t)sqlite3_column_bytes(stmt, 0));
        if (!str_eq(stored_name, name)) {
            fprintf(stderr, "db: migration %04d was applied as '%.*s' but its file is now '%s'; "
                            "applied migrations must not be renamed or renumbered\n",
                    m->version, STR_ARG(stored_name), m->file_name);
            ok = false;
        }
    }
    sqlite3_finalize(stmt);
    return ok;
}

static bool record_applied(sqlite3 *conn, Migration const *m) {
    sqlite3_stmt *stmt = NULL;
    Str name = migration_name(m);
    bool ok = sqlite3_prepare_v2(conn, "INSERT INTO schema_migrations (version, name) VALUES (?1, ?2)",
                                 -1, &stmt, NULL) == SQLITE_OK &&
              sqlite3_bind_int(stmt, 1, m->version) == SQLITE_OK &&
              sqlite3_bind_text(stmt, 2, name.data, (int)name.len, SQLITE_TRANSIENT) == SQLITE_OK &&
              sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) {
        fprintf(stderr, "db: cannot record migration '%s': %s\n", m->file_name, sqlite3_errmsg(conn));
    }
    sqlite3_finalize(stmt);
    return ok;
}

static Migration const *find_version(Migration const *list, int count, int version) {
    for (int i = 0; i < count; i++) {
        if (list[i].version == version) {
            return &list[i];
        }
    }
    return NULL;
}

/*
 * Creates schema_migrations on first use. A database from before migration
 * files (PRAGMA user_version set) is adopted instead of being migrated again.
 * Runs inside the caller's transaction.
 */
static bool create_migrations_table(sqlite3 *conn, Migration const *list, int count) {
    int legacy_version;
    if (!query_int(conn, "PRAGMA user_version", &legacy_version) ||
        !exec(conn,
              "CREATE TABLE schema_migrations ("
              "    version    INTEGER PRIMARY KEY,"
              "    name       TEXT NOT NULL,"
              "    applied_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))"
              ");",
              "creating schema_migrations")) {
        return false;
    }
    if (legacy_version == 0) {
        return true;
    }
    if (legacy_version != LEGACY_SCHEMA_VERSION) {
        fprintf(stderr, "db: database has old schema version %d; only version %d can be adopted\n",
                legacy_version, LEGACY_SCHEMA_VERSION);
        return false;
    }
    for (int version = 1; version <= LEGACY_SCHEMA_VERSION; version++) {
        Migration const *m = find_version(list, count, version);
        if (!m) {
            fprintf(stderr, "db: cannot adopt old database: migration %04d is missing\n", version);
            return false;
        }
        if (!record_applied(conn, m)) {
            return false;
        }
        fprintf(stderr, "db: adopted existing schema as %s\n", m->file_name);
    }
    return exec(conn, "PRAGMA user_version = 0;", "clearing old schema version");
}

static bool ensure_migrations_table(sqlite3 *conn, Migration const *list, int count) {
    int exists;
    char const *exists_sql =
        "SELECT count(*) FROM sqlite_schema WHERE type = 'table' AND name = 'schema_migrations'";
    if (!query_int(conn, exists_sql, &exists)) {
        return false;
    }
    if (exists) {
        return true;
    }
    // Checked again under the write lock: another process may be doing the same.
    bool ok = exec(conn, "BEGIN IMMEDIATE;", "starting transaction") &&
              query_int(conn, exists_sql, &exists) &&
              (exists || create_migrations_table(conn, list, count)) &&
              exec(conn, "COMMIT;", "committing schema_migrations");
    if (!ok) {
        exec(conn, "ROLLBACK;", "rolling back");
    }
    return ok;
}

static bool apply(sqlite3 *conn, char const *dir, Migration const *m) {
    char *sql = read_sql(dir, m);
    if (!sql) {
        return false;
    }
    bool applied = false;
    bool ok = exec(conn, "BEGIN IMMEDIATE;", "starting transaction") &&
              is_applied(conn, m->version, &applied) &&
              (applied || (exec(conn, sql, m->file_name) && record_applied(conn, m))) &&
              exec(conn, "COMMIT;", m->file_name);
    if (!ok) {
        exec(conn, "ROLLBACK;", "rolling back");
    } else if (!applied) {
        fprintf(stderr, "db: applied migration %s\n", m->file_name);
    }
    sqlite3_free(sql);
    return ok;
}

bool migrate(sqlite3 *conn, char const *dir) {
    Migration list[MAX_MIGRATIONS];
    int count;
    if (!load_migrations(dir, list, &count) || !ensure_migrations_table(conn, list, count)) {
        return false;
    }

    int current;
    if (!query_int(conn, "SELECT coalesce(max(version), 0) FROM schema_migrations", &current)) {
        return false;
    }
    int newest = list[count - 1].version;
    if (current > newest) {
        fprintf(stderr, "db: database is at migration %04d, newer than the newest file in '%s' (%04d)\n",
                current, dir, newest);
        return false;
    }

    for (int i = 0; i < count; i++) {
        bool applied;
        if (!is_applied(conn, list[i].version, &applied)) {
            return false;
        }
        if (applied) {
            if (!check_applied_name(conn, &list[i])) {
                return false;
            }
            continue;
        }
        // Running it now would put it after migrations that were written later.
        if (list[i].version < current) {
            fprintf(stderr, "db: %s was never applied but the database is already at %04d; "
                            "give it a number above %04d\n",
                    list[i].file_name, current, current);
            return false;
        }
        if (!apply(conn, dir, &list[i])) {
            return false;
        }
    }

    Migration const *latest = &list[count - 1];
    fprintf(stderr, "db: schema at migration %s\n", latest->file_name);
    return true;
}
