#include "migrate.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sql.h"
#include "str.h"

enum {
    /** Most migration files in the directory. */
    MAX_MIGRATIONS = 256,
    /** Longest migration file name, with its terminator, in bytes. */
    MAX_FILE_NAME = 128,
    /** Longest path of a migration file, with its terminator, in bytes. */
    MAX_FILE_PATH = 1024,
    /** Largest migration file, in bytes. */
    MAX_SQL_SIZE = 4 * 1024 * 1024,
    /** Digits of the version number at the start of a file name. */
    VERSION_DIGITS = 4,
    /**
     * Before migration files existed, the schema was compiled into the server and
     * PRAGMA user_version counted the steps. Version 2 is exactly what files 0001
     * and 0002 create, so such a database is adopted by recording those two as
     * applied without running them.
     */
    LEGACY_SCHEMA_VERSION = 2,
};

typedef struct {
    int    version;
    /** Storage for the file name; read it with file_name(). */
    char   file_name_buf[MAX_FILE_NAME];
    size_t file_name_len;
    /** Length of the name part ("create_users_table"). */
    size_t name_len;
} Migration;

/**
 * The text of a migration file, read into memory from SQLite's heap.
 */
typedef struct {
    char  *bytes;  /**< Freed with sqlite3_free(). */
    size_t len;
} Sql_Text;

typedef enum {
    NAME_IGNORED,  /**< Not a .sql file; not a migration. */
    NAME_VALID,
    NAME_INVALID,  /**< A .sql file with a malformed name. */
} Name_Kind;

// The file name, like "0001__create_users_table.sql".
static Str file_name(Migration const *m) {
    return str_make(m->file_name_buf, m->file_name_len);
}

// The name part of the file name, like "create_users_table".
static Str migration_name(Migration const *m) {
    return str_slice(file_name(m), VERSION_DIGITS + 2, VERSION_DIGITS + 2 + m->name_len);
}

static Name_Kind classify_file_name(Str name_in_dir, int *version, size_t *name_len) {
    assert(version != NULL);
    assert(name_len != NULL);
    Str suffix = STR(".sql");
    if (!str_ends_with(name_in_dir, suffix)) {
        return NAME_IGNORED;
    }
    Str stem = str_slice(name_in_dir, 0, name_in_dir.len - suffix.len);
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

static bool load_migrations(Str dir, Migration *list, int *count) {
    assert(list != NULL);
    assert(count != NULL);
    char dir_z[MAX_FILE_PATH];  // opendir() needs a NUL-terminated path.
    if (!str_to_cstr(dir, dir_z, sizeof dir_z)) {
        fprintf(stderr, "db: migrations directory path is empty or too long\n");
        return false;
    }
    DIR *d = opendir(dir_z);
    if (!d) {
        fprintf(stderr, "db: cannot open migrations directory '%.*s': %s\n",
                STR_ARG(dir), strerror(errno));
        return false;
    }
    *count = 0;
    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir(d)) != NULL) {
        Str name_in_dir = str_from_cstr(entry->d_name);
        int version;
        size_t name_len;
        Name_Kind kind = classify_file_name(name_in_dir, &version, &name_len);
        if (kind == NAME_IGNORED) {
            continue;
        }
        if (kind == NAME_INVALID) {
            fprintf(stderr,
                    "db: migration '%.*s' must be named like 0001__create_users_table.sql\n",
                    STR_ARG(name_in_dir));
            ok = false;
        } else if (*count == MAX_MIGRATIONS || name_in_dir.len >= MAX_FILE_NAME) {
            fprintf(stderr, "db: too many migrations, or a name too long, at '%.*s'\n",
                    STR_ARG(name_in_dir));
            ok = false;
        } else {
            Migration *m = &list[*count];
            m->version = version;
            m->name_len = name_len;
            m->file_name_len = name_in_dir.len;
            memcpy(m->file_name_buf, name_in_dir.data, name_in_dir.len);
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
            fprintf(stderr, "db: migrations '%.*s' and '%.*s' share a number\n",
                    STR_ARG(file_name(&list[i - 1])), STR_ARG(file_name(&list[i])));
            return false;
        }
    }
    if (*count == 0) {
        fprintf(stderr, "db: no migrations found in '%.*s'\n", STR_ARG(dir));
        return false;
    }
    return true;
}

/*
 * Reads a migration file into memory from SQLite's heap. Free the bytes with
 * sqlite3_free().
 */
static bool read_sql(Str dir, Migration const *m, Sql_Text *sql) {
    assert(sql != NULL);
    char path_buf[MAX_FILE_PATH];
    Str_Builder path;
    str_builder_init(&path, path_buf, sizeof path_buf);
    str_builder_append(&path, dir);
    str_builder_append(&path, STR("/"));
    str_builder_append(&path, file_name(m));
    char const *path_z = str_builder_cstr(&path);  // open() needs a NUL-terminated path.
    if (!path_z) {
        fprintf(stderr, "db: path to '%.*s' is too long\n", STR_ARG(file_name(m)));
        return false;
    }

    int fd = open(path_z, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        fprintf(stderr, "db: cannot read '%s': %s\n", path_z, strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    if (st.st_size > MAX_SQL_SIZE) {
        fprintf(stderr, "db: '%s' is larger than %d bytes\n", path_z, MAX_SQL_SIZE);
        close(fd);
        return false;
    }

    size_t size = (size_t)st.st_size;
    char *bytes = sqlite3_malloc64(size > 0 ? size : 1);
    size_t got = 0;
    while (bytes && got < size) {
        ssize_t r = read(fd, bytes + got, size - got);
        if (r > 0) {
            got += (size_t)r;
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(fd);
    if (!bytes || got != size) {
        fprintf(stderr, "db: cannot read '%s'\n", path_z);
        sqlite3_free(bytes);
        return false;
    }
    sql->bytes = bytes;
    sql->len = size;
    return true;
}

// Runs a query that returns one integer, such as a count or a version.
static bool query_int(sqlite3 *conn, Str sql, int *value) {
    assert(value != NULL);
    sqlite3_stmt *stmt = NULL;
    bool ok = sql_prepare(conn, &stmt, sql, false) && sqlite3_step(stmt) == SQLITE_ROW;
    if (ok) {
        *value = sqlite3_column_int(stmt, 0);
    } else {
        fprintf(stderr, "db: %.*s: %s\n", STR_ARG(sql), sqlite3_errmsg(conn));
    }
    sqlite3_finalize(stmt);
    return ok;
}

static bool is_applied(sqlite3 *conn, int version, bool *applied) {
    assert(applied != NULL);
    sqlite3_stmt *stmt = NULL;
    Str const sql = STR("SELECT 1 FROM schema_migrations WHERE version = ?1");
    bool ok = sql_prepare(conn, &stmt, sql, false);
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
    Str const sql = STR("SELECT name FROM schema_migrations WHERE version = ?1");
    bool ok = sql_prepare(conn, &stmt, sql, false) &&
              sqlite3_bind_int(stmt, 1, m->version) == SQLITE_OK &&
              sqlite3_step(stmt) == SQLITE_ROW;
    if (!ok) {
        fprintf(stderr, "db: cannot read schema_migrations: %s\n", sqlite3_errmsg(conn));
    } else {
        Str stored_name = sql_column_str(stmt, 0);
        if (!str_eq(stored_name, migration_name(m))) {
            fprintf(stderr, "db: migration %04d was applied as '%.*s' but its file is now '%.*s'; "
                            "applied migrations must not be renamed or renumbered\n",
                    m->version, STR_ARG(stored_name), STR_ARG(file_name(m)));
            ok = false;
        }
    }
    sqlite3_finalize(stmt);
    return ok;
}

static bool record_applied(sqlite3 *conn, Migration const *m) {
    sqlite3_stmt *stmt = NULL;
    Str const sql = STR("INSERT INTO schema_migrations (version, name) VALUES (?1, ?2)");
    bool ok = sql_prepare(conn, &stmt, sql, false) &&
              sqlite3_bind_int(stmt, 1, m->version) == SQLITE_OK &&
              sql_bind_str(stmt, 2, migration_name(m)) &&
              sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) {
        fprintf(stderr, "db: cannot record migration '%.*s': %s\n",
                STR_ARG(file_name(m)), sqlite3_errmsg(conn));
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
    if (!query_int(conn, STR("PRAGMA user_version"), &legacy_version) ||
        !sql_exec(conn,
                  STR("CREATE TABLE schema_migrations ("
                      "    version    INTEGER PRIMARY KEY,"
                      "    name       TEXT NOT NULL,"
                      "    applied_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))"
                      ");"),
                  STR("creating schema_migrations"))) {
        return false;
    }
    if (legacy_version == 0) {
        return true;
    }
    if (legacy_version != LEGACY_SCHEMA_VERSION) {
        fprintf(stderr,
                "db: database has old schema version %d; only version %d can be adopted\n",
                legacy_version, LEGACY_SCHEMA_VERSION);
        return false;
    }
    for (int version = 1; version <= LEGACY_SCHEMA_VERSION; version++) {
        Migration const *m = find_version(list, count, version);
        if (!m) {
            fprintf(stderr, "db: cannot adopt old database: migration %04d is missing\n",
                version);
            return false;
        }
        if (!record_applied(conn, m)) {
            return false;
        }
        fprintf(stderr, "db: adopted existing schema as %.*s\n", STR_ARG(file_name(m)));
    }
    return sql_exec(conn, STR("PRAGMA user_version = 0;"), STR("clearing old schema version"));
}

static bool ensure_migrations_table(sqlite3 *conn, Migration const *list, int count) {
    int exists;
    Str exists_sql = STR(
        "SELECT count(*) FROM sqlite_schema WHERE type = 'table' AND name = 'schema_migrations'");
    if (!query_int(conn, exists_sql, &exists)) {
        return false;
    }
    if (exists) {
        return true;
    }
    // Checked again under the write lock: another process may be doing the same.
    bool ok = sql_exec(conn, STR("BEGIN IMMEDIATE;"), STR("starting transaction")) &&
              query_int(conn, exists_sql, &exists) &&
              (exists || create_migrations_table(conn, list, count)) &&
              sql_exec(conn, STR("COMMIT;"), STR("committing schema_migrations"));
    if (!ok) {
        sql_exec(conn, STR("ROLLBACK;"), STR("rolling back"));
    }
    return ok;
}

static bool apply(sqlite3 *conn, Str dir, Migration const *m) {
    Sql_Text text;
    if (!read_sql(dir, m, &text)) {
        return false;
    }
    Str const sql = str_make(text.bytes, text.len);
    bool applied = false;
    bool ok = sql_exec(conn, STR("BEGIN IMMEDIATE;"), STR("starting transaction")) &&
              is_applied(conn, m->version, &applied) &&
              (applied || (sql_exec(conn, sql, file_name(m)) && record_applied(conn, m))) &&
              sql_exec(conn, STR("COMMIT;"), file_name(m));
    if (!ok) {
        sql_exec(conn, STR("ROLLBACK;"), STR("rolling back"));
    } else if (!applied) {
        fprintf(stderr, "db: applied migration %.*s\n", STR_ARG(file_name(m)));
    }
    sqlite3_free(text.bytes);
    return ok;
}

bool migrate(sqlite3 *conn, Str dir) {
    Migration list[MAX_MIGRATIONS];
    int count;
    if (!load_migrations(dir, list, &count) || !ensure_migrations_table(conn, list, count)) {
        return false;
    }

    int current;
    Str const current_sql = STR("SELECT coalesce(max(version), 0) FROM schema_migrations");
    if (!query_int(conn, current_sql, &current)) {
        return false;
    }
    int newest = list[count - 1].version;
    if (current > newest) {
        fprintf(stderr,
                "db: database is at migration %04d, newer than the newest file in '%.*s' (%04d)\n",
                current, STR_ARG(dir), newest);
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
            fprintf(stderr, "db: %.*s was never applied but the database is already at %04d; "
                            "give it a number above %04d\n",
                    STR_ARG(file_name(&list[i])), current, current);
            return false;
        }
        if (!apply(conn, dir, &list[i])) {
            return false;
        }
    }

    fprintf(stderr, "db: schema at migration %.*s\n", STR_ARG(file_name(&list[count - 1])));
    return true;
}
