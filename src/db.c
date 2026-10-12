#include "db.h"

#include <stdio.h>

#include "buddy.h"

static Buddy_Heap DB_HEAP;

/*
 * The schema enforces the rules for recipe fields, so invalid input from the
 * API surfaces as a constraint failure no matter which statement wrote it.
 */
static char const SCHEMA_SQL[] =
    "PRAGMA journal_mode = WAL;"
    "CREATE TABLE IF NOT EXISTS recipes ("
    "    id          INTEGER PRIMARY KEY,"
    "    name        TEXT NOT NULL"
    "                CHECK (typeof(name) = 'text' AND length(trim(name)) BETWEEN 1 AND 200),"
    "    image       TEXT NOT NULL"
    "                CHECK (typeof(image) = 'text' AND length(image) <= 500),"
    "    description TEXT NOT NULL"
    "                CHECK (typeof(description) = 'text' AND length(description) <= 2000)"
    ");";

static char const SEED_SQL[] =
    "INSERT INTO recipes (name, image, description) VALUES"
    " ('Laventelisaippua', '/assets/recipes/laventelisaippua.jpg',"
    "  'Rauhoittava käsintehty saippua kuivalle iholle.'),"
    " ('Yrttivoide kuivalle iholle', '/assets/recipes/yrttivoide.jpg',"
    "  'Puutarhan yrteistä uutettu, hoitava perusvoide.'),"
    " ('Kotitekoinen marjahillo', '/assets/recipes/marjahillo.jpg',"
    "  'Kesän marjasadosta keitetty, vähäsokerinen hillo.'),"
    " ('Ruusuvesitoner', '/assets/recipes/ruusuvesitoner.jpg',"
    "  'Virkistävä kasvovesi tuoreista ruusunlehdistä.'),"
    " ('Hapanjuurileipä', '/assets/recipes/hapanjuurileipa.jpg',"
    "  'Rapeakuorinen leipä omalla juuritaikinalla.'),"
    " ('Kalendulavoide', '/assets/recipes/kalendulavoide.jpg',"
    "  'Ärsytystä lievittävä voide kehäkukan uutteesta.'),"
    " ('Omena-kanelihillo', '/assets/recipes/omenahillo.jpg',"
    "  'Syksyn omenista, kanelista ja tähtianiksesta.'),"
    " ('Yrttisuola', '/assets/recipes/yrttisuola.jpg',"
    "  'Kuivatuista yrteistä sekoitettu mauste-suola.');";

// SQLite memory methods backed by the buddy heap.

static void *mem_malloc(int size) {
    return buddy_alloc(&DB_HEAP, (size_t)size);
}

static void mem_free(void *ptr) {
    buddy_free(&DB_HEAP, ptr);
}

static void *mem_realloc(void *ptr, int size) {
    return buddy_realloc(&DB_HEAP, ptr, (size_t)size);
}

static int mem_size(void *ptr) {
    return ptr ? (int)buddy_usable_size(ptr) : 0;
}

static int mem_roundup(int size) {
    size_t rounded = buddy_round_up(&DB_HEAP, (size_t)size);
    return rounded > 0 ? (int)rounded : size;  // Too big: let the allocation fail.
}

static int mem_init(void *app_data) {
    (void)app_data;
    return SQLITE_OK;
}

static void mem_shutdown(void *app_data) {
    (void)app_data;
}

bool db_global_init(void *heap_mem, size_t heap_size) {
    if (!buddy_init(&DB_HEAP, heap_mem, heap_size)) {
        fputs("db: invalid heap size\n", stderr);
        return false;
    }
    static sqlite3_mem_methods const MEM_METHODS = {
        .xMalloc = mem_malloc,
        .xFree = mem_free,
        .xRealloc = mem_realloc,
        .xSize = mem_size,
        .xRoundup = mem_roundup,
        .xInit = mem_init,
        .xShutdown = mem_shutdown,
        .pAppData = NULL,
    };
    /*
     * Each connection is used by exactly one thread, so SQLite's own
     * per-connection mutexes are not needed.
     */
    if (sqlite3_config(SQLITE_CONFIG_MALLOC, &MEM_METHODS) != SQLITE_OK ||
        sqlite3_config(SQLITE_CONFIG_MULTITHREAD) != SQLITE_OK ||
        sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 0) != SQLITE_OK ||
        sqlite3_initialize() != SQLITE_OK) {
        fputs("db: cannot configure SQLite\n", stderr);
        return false;
    }
    return true;
}

static bool exec(sqlite3 *conn, char const *sql) {
    char *error = NULL;
    if (sqlite3_exec(conn, sql, NULL, NULL, &error) != SQLITE_OK) {
        fprintf(stderr, "db: %s\n", error ? error : sqlite3_errmsg(conn));
        sqlite3_free(error);
        return false;
    }
    return true;
}

static bool open_connection(sqlite3 **conn, char const *path) {
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
    if (sqlite3_open_v2(path, conn, flags, NULL) != SQLITE_OK) {
        fprintf(stderr, "db: cannot open '%s': %s\n", path,
                *conn ? sqlite3_errmsg(*conn) : "out of memory");
        sqlite3_close(*conn);
        *conn = NULL;
        return false;
    }
    sqlite3_busy_timeout(*conn, 5000);
    return exec(*conn, "PRAGMA synchronous = NORMAL; PRAGMA cache_size = -512;");
}

bool db_prepare_file(char const *path) {
    sqlite3 *conn;
    if (!open_connection(&conn, path)) {
        return false;
    }
    bool ok = exec(conn, SCHEMA_SQL);

    sqlite3_stmt *count = NULL;
    if (ok && sqlite3_prepare_v2(conn, "SELECT count(*) FROM recipes", -1, &count, NULL) != SQLITE_OK) {
        ok = false;
    }
    if (ok && sqlite3_step(count) == SQLITE_ROW && sqlite3_column_int64(count, 0) == 0) {
        ok = exec(conn, SEED_SQL);
    }
    sqlite3_finalize(count);
    sqlite3_close(conn);
    return ok;
}

/**
 * SQL for a recipe field taken from the JSON in parameter ?param: its value
 * when it is a JSON string, missing_value when the key is absent, and NULL
 * (which NOT NULL rejects) for any other JSON type. Without this, TEXT column
 * affinity would quietly turn a JSON number into text.
 */
#define TEXT_FIELD(param, key, missing_value)                            \
    "CASE coalesce(json_type(?" param ", '$." key "'), 'missing')"       \
    " WHEN 'text' THEN json_extract(?" param ", '$." key "')"            \
    " WHEN 'missing' THEN " missing_value " END"

static bool prepare(Db *db, sqlite3_stmt **stmt, char const *sql) {
    if (sqlite3_prepare_v3(db->conn, sql, -1, SQLITE_PREPARE_PERSISTENT, stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "db: cannot prepare statement: %s\n", sqlite3_errmsg(db->conn));
        return false;
    }
    return true;
}

bool db_open(Db *db, char const *path) {
    *db = (Db) {
        .conn = NULL,
        .is_json_object = NULL,
        .list_recipes = NULL,
        .get_recipe = NULL,
        .insert_recipe = NULL,
        .update_recipe = NULL,
        .delete_recipe = NULL,
        .parse_login = NULL,
    };
    if (!open_connection(&db->conn, path)) {
        return false;
    }
    bool ok =
        prepare(db, &db->is_json_object,
                "SELECT CASE WHEN json_valid(?1) THEN json_type(?1) = 'object' ELSE 0 END") &&
        prepare(db, &db->list_recipes,
                "SELECT json_group_array(json_object("
                "    'id', id, 'name', name, 'image', image, 'description', description))"
                " FROM (SELECT * FROM recipes ORDER BY id)") &&
        prepare(db, &db->get_recipe,
                "SELECT json_object("
                "    'id', id, 'name', name, 'image', image, 'description', description)"
                " FROM recipes WHERE id = ?1") &&
        prepare(db, &db->insert_recipe,
                "INSERT INTO recipes (name, image, description) VALUES ("
                TEXT_FIELD("1", "name", "NULL") ", "
                TEXT_FIELD("1", "image", "''") ", "
                TEXT_FIELD("1", "description", "''") ")"
                " RETURNING id") &&
        prepare(db, &db->update_recipe,
                "UPDATE recipes SET"
                " name = " TEXT_FIELD("2", "name", "NULL") ","
                " image = " TEXT_FIELD("2", "image", "''") ","
                " description = " TEXT_FIELD("2", "description", "''")
                " WHERE id = ?1") &&
        prepare(db, &db->delete_recipe, "DELETE FROM recipes WHERE id = ?1") &&
        prepare(db, &db->parse_login,
                "SELECT json_extract(?1, '$.username'), json_extract(?1, '$.password')");
    if (!ok) {
        db_close(db);
    }
    return ok;
}

void db_close(Db *db) {
    sqlite3_finalize(db->is_json_object);
    sqlite3_finalize(db->list_recipes);
    sqlite3_finalize(db->get_recipe);
    sqlite3_finalize(db->insert_recipe);
    sqlite3_finalize(db->update_recipe);
    sqlite3_finalize(db->delete_recipe);
    sqlite3_finalize(db->parse_login);
    sqlite3_close(db->conn);
    db->conn = NULL;
}

static Db_Status status_from(int rc) {
    switch (rc & 0xff) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return DB_BUSY;
    case SQLITE_CONSTRAINT:
    case SQLITE_TOOBIG:
    case SQLITE_MISMATCH:
        return DB_INVALID;
    default:
        return DB_ERROR;
    }
}

static void finish(sqlite3_stmt *stmt) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
}

static bool bind_str(sqlite3_stmt *stmt, int index, Str s) {
    return sqlite3_bind_text(stmt, index, s.len > 0 ? s.data : "", (int)s.len, SQLITE_STATIC) == SQLITE_OK;
}

static Db_Status check_json_object(Db *db, Str json) {
    if (json.len > 64 * 1024 || !bind_str(db->is_json_object, 1, json)) {
        finish(db->is_json_object);
        return DB_INVALID;
    }
    int rc = sqlite3_step(db->is_json_object);
    Db_Status status = DB_ERROR;
    if (rc == SQLITE_ROW) {
        status = sqlite3_column_int(db->is_json_object, 0) ? DB_OK : DB_INVALID;
    } else {
        status = status_from(rc);
    }
    finish(db->is_json_object);
    return status;
}

// Appends the single text column of a one-row query.
static Db_Status append_text_result(sqlite3_stmt *stmt, Str_Builder *out) {
    int rc = sqlite3_step(stmt);
    Db_Status status;
    if (rc == SQLITE_ROW) {
        char const *text = (char const *)sqlite3_column_text(stmt, 0);
        int len = sqlite3_column_bytes(stmt, 0);
        if (!text) {
            status = DB_ERROR;
        } else if (str_builder_append(out, str_make(text, (size_t)len))) {
            status = DB_OK;
        } else {
            status = DB_TOO_LARGE;
        }
    } else if (rc == SQLITE_DONE) {
        status = DB_NOT_FOUND;
    } else {
        status = status_from(rc);
    }
    finish(stmt);
    return status;
}

Db_Status db_list_recipes_json(Db *db, Str_Builder *out) {
    return append_text_result(db->list_recipes, out);
}

Db_Status db_get_recipe_json(Db *db, int64_t id, Str_Builder *out) {
    sqlite3_bind_int64(db->get_recipe, 1, id);
    return append_text_result(db->get_recipe, out);
}

Db_Status db_create_recipe(Db *db, Str json, int64_t *id) {
    Db_Status status = check_json_object(db, json);
    if (status != DB_OK) {
        return status;
    }
    bind_str(db->insert_recipe, 1, json);
    int rc = sqlite3_step(db->insert_recipe);
    if (rc == SQLITE_ROW) {
        *id = sqlite3_column_int64(db->insert_recipe, 0);
        rc = sqlite3_step(db->insert_recipe);  // Runs the statement to completion.
    }
    status = rc == SQLITE_DONE ? DB_OK : status_from(rc);
    finish(db->insert_recipe);
    return status;
}

Db_Status db_update_recipe(Db *db, int64_t id, Str json) {
    Db_Status status = check_json_object(db, json);
    if (status != DB_OK) {
        return status;
    }
    sqlite3_bind_int64(db->update_recipe, 1, id);
    bind_str(db->update_recipe, 2, json);
    int rc = sqlite3_step(db->update_recipe);
    if (rc != SQLITE_DONE) {
        status = status_from(rc);
    } else if (sqlite3_changes(db->conn) == 0) {
        status = DB_NOT_FOUND;
    } else {
        status = DB_OK;
    }
    finish(db->update_recipe);
    return status;
}

Db_Status db_delete_recipe(Db *db, int64_t id) {
    sqlite3_bind_int64(db->delete_recipe, 1, id);
    int rc = sqlite3_step(db->delete_recipe);
    Db_Status status;
    if (rc != SQLITE_DONE) {
        status = status_from(rc);
    } else if (sqlite3_changes(db->conn) == 0) {
        status = DB_NOT_FOUND;
    } else {
        status = DB_OK;
    }
    finish(db->delete_recipe);
    return status;
}

static bool append_text_column(sqlite3_stmt *stmt, int column, Str_Builder *out) {
    if (sqlite3_column_type(stmt, column) != SQLITE_TEXT) {
        return false;
    }
    char const *text = (char const *)sqlite3_column_text(stmt, column);
    int len = sqlite3_column_bytes(stmt, column);
    return text && str_builder_append(out, str_make(text, (size_t)len));
}

Db_Status db_parse_login(Db *db, Str json, Str_Builder *username, Str_Builder *password) {
    Db_Status status = check_json_object(db, json);
    if (status != DB_OK) {
        return status;
    }
    bind_str(db->parse_login, 1, json);
    int rc = sqlite3_step(db->parse_login);
    if (rc != SQLITE_ROW) {
        status = status_from(rc);
    } else if (!append_text_column(db->parse_login, 0, username) ||
               !append_text_column(db->parse_login, 1, password)) {
        status = DB_INVALID;
    } else {
        status = DB_OK;
    }
    finish(db->parse_login);
    return status;
}
