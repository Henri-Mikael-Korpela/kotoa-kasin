#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db.h"
#include "buddy.h"
#include "migrate.h"
#include "sql.h"

enum {
    /** Longest database path, with its terminator, in bytes. */
    MAX_FILE_PATH = 1024,
    /** Login attempts and clients are kept this long, in seconds (90 days). */
    LOGIN_RETENTION_SEC = 90 * 24 * 60 * 60,
    /** Largest JSON request body that is looked into, in bytes. */
    MAX_JSON_SIZE = 1024 * 1024,
};

static Buddy_Heap DB_HEAP;

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
    if (ptr) {
        return (int)buddy_usable_size(ptr);
    }
    return 0;
}

static int mem_roundup(int size) {
    size_t rounded = buddy_round_up(&DB_HEAP, (size_t)size);
    if (rounded > 0) {
        return (int)rounded;
    }
    return size;  // Too big: let the allocation fail.
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

/*
 * SQLite's "unix-dotfile" VFS locks a database by creating a directory called
 * "<database>.lock" and removing it afterwards. Creating a directory is atomic
 * on network shares that cannot do POSIX byte-range locks. It also keeps other
 * processes out, such as the old revision while a new one starts.
 */
static char const *vfs_for(DB_Storage storage) {
    // sqlite3_open_v2() takes a C string; NULL selects the default VFS.
    if (storage == DB_STORAGE_SHARE) {
        return "unix-dotfile";
    }
    return NULL;
}

static bool open_connection(sqlite3 **conn, Str path, DB_Storage storage) {
    assert(conn != NULL);
    char path_z[MAX_FILE_PATH];  // sqlite3_open_v2() needs a NUL-terminated path.
    if (!str_to_cstr(path, path_z, sizeof path_z)) {
        fprintf(stderr, "db: database path is empty or too long\n");
        return false;
    }
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
    if (sqlite3_open_v2(path_z, conn, flags, vfs_for(storage)) != SQLITE_OK) {
        fprintf(stderr, "db: cannot open '%.*s': %s\n", STR_ARG(path),
                *conn ? sqlite3_errmsg(*conn) : "out of memory");
        sqlite3_close(*conn);
        *conn = NULL;
        return false;
    }
    sqlite3_busy_timeout(*conn, 5000);
    sqlite3_extended_result_codes(*conn, 1);  // Tells unique and foreign key failures apart.
    return sql_exec(*conn, STR("PRAGMA synchronous = NORMAL; PRAGMA cache_size = -512;"),
                    STR("configuring connection"));
}

/*
 * A crash while a lock is held leaves the lock directory behind, which would
 * block every later start. Wait briefly in case a live process (an old revision
 * shutting down) is about to release it, then remove it.
 */
static void clear_stale_lock(Str path) {
    char lock_buf[MAX_FILE_PATH];
    Str_Builder lock;
    str_builder_init(&lock, lock_buf, sizeof lock_buf);
    str_builder_append(&lock, path);
    str_builder_append(&lock, STR(".lock"));
    char const *lock_path = str_builder_cstr(&lock);  // stat() and rmdir() need C strings.
    if (!lock_path) {
        return;
    }
    struct stat st;
    for (int i = 0; i < 30 && stat(lock_path, &st) == 0; i++) {
        usleep(100 * 1000);
    }
    if (stat(lock_path, &st) == 0) {
        fprintf(stderr, "db: removing stale lock %s\n", lock_path);
        if (rmdir(lock_path) != 0) {
            unlink(lock_path);
        }
    }
}

bool db_prepare_file(Str path, DB_Storage storage, Str migrations_dir) {
    if (storage == DB_STORAGE_SHARE) {
        clear_stale_lock(path);
    }
    sqlite3 *conn;
    if (!open_connection(&conn, path, storage)) {
        return false;
    }
    // Table rebuilds in migrations need foreign keys off; checked once at the end.
    Str journal_mode = STR("PRAGMA journal_mode = DELETE;");
    if (storage == DB_STORAGE_LOCAL) {
        journal_mode = STR("PRAGMA journal_mode = WAL;");
    }
    bool ok = sql_exec(conn, journal_mode, STR("setting journal mode")) &&
              sql_exec(conn, STR("PRAGMA foreign_keys = OFF;"), STR("disabling foreign keys"));

    ok = ok && migrate(conn, migrations_dir);

    sqlite3_stmt *check = NULL;
    if (ok && sql_prepare(conn, &check, STR("PRAGMA foreign_key_check"), false) &&
        sqlite3_step(check) == SQLITE_ROW) {
        fputs("db: foreign key check failed\n", stderr);
        ok = false;
    }
    sqlite3_finalize(check);
    sqlite3_close(conn);
    return ok;
}

/*
 * The macros below build SQL text. They have to be macros: the compiler joins
 * adjacent string literals into one statement, which a constant array of
 * characters cannot do.
 */

/**
 * SQL for a field taken from the JSON in parameter ?param: its value when it
 * is a JSON string, missing_value when the key is absent, and NULL (which NOT
 * NULL rejects) for any other JSON type. Without this, TEXT column affinity
 * would quietly turn a JSON number into text.
 */
#define TEXT_FIELD(param, key, missing_value)                            \
    "CASE coalesce(json_type(?" param ", '$." key "'), 'missing')"       \
    " WHEN 'text' THEN json_extract(?" param ", '$." key "')"            \
    " WHEN 'missing' THEN " missing_value " END"

/**
 * SQL for a required integer field from the JSON in parameter ?param, or NULL
 * when it is missing or not an integer.
 */
#define INTEGER_FIELD(param, key)                                        \
    "CASE json_type(?" param ", '$." key "')"                            \
    " WHEN 'integer' THEN json_extract(?" param ", '$." key "') END"

// Columns of a recipe joined with its category, in the shape the API returns.
#define RECIPE_JSON                                                      \
    "json_object('id', r.id, 'slug', r.slug, 'name', r.name, 'image', r.image," \
    " 'description', r.description, 'category_id', c.id, 'category', c.name,"  \
    " 'created_at', r.created_at, 'updated_at', r.updated_at)"

/*
 * Plain text for list views, from the start of a recipe description: the text
 * of its first paragraph block, or the whole description when it has no blocks
 * (plain text from before the editor). Unescapes the two sequences the editor
 * escapes in block text. A '<!-- block:paragraph -->' marker is 24 bytes long.
 */
#define PARAGRAPH_OPEN "'<!-- block:paragraph -->'"
#define AFTER_PARAGRAPH_OPEN                                             \
    "substr(r.description, instr(r.description, " PARAGRAPH_OPEN ") + 24)"
#define RECIPE_EXCERPT                                                   \
    "substr(trim(replace(replace(CASE"                                   \
    " WHEN instr(r.description, " PARAGRAPH_OPEN ") > 0 THEN"            \
    "  substr(" AFTER_PARAGRAPH_OPEN ", 1,"                              \
    "   instr(" AFTER_PARAGRAPH_OPEN ", '<!-- /block:paragraph -->') - 1)" \
    " WHEN instr(r.description, '<!-- block:') = 0 THEN r.description"   \
    " ELSE '' END, '<\\!--', '<!--'), '\\\\', '\\'), char(32, 9, 10, 13)), 1, 300)"

// Columns of a recipe in list views.
#define RECIPE_SUMMARY_JSON                                              \
    "json_object('id', r.id, 'slug', r.slug, 'name', r.name, 'image', r.image," \
    " 'excerpt', " RECIPE_EXCERPT ", 'category_id', c.id, 'category', c.name,"  \
    " 'created_at', r.created_at, 'updated_at', r.updated_at)"

// Where an image is served from, and how many recipes show it.
#define IMAGE_URL "('/uploads/' || i.id || '.' || i.extension)"
#define IMAGE_IS_USED_BY_RECIPE                                          \
    " WHERE image = " IMAGE_URL " OR instr(description, " IMAGE_URL ") > 0"
#define IMAGE_USE_COUNT                                                  \
    "(SELECT count(*) FROM recipes" IMAGE_IS_USED_BY_RECIPE ")"

// The recipes that show an image, by name, as a JSON array of {id, name}.
#define IMAGE_RECIPES                                                    \
    "(SELECT json_group_array(json_object('id', u.id, 'name', u.name)) FROM" \
    " (SELECT id, name FROM recipes" IMAGE_IS_USED_BY_RECIPE " ORDER BY name) u)"

// A category with the number of recipes in it, in the shape the API returns.
#define CATEGORY_JSON                                                    \
    "json_object('id', c.id, 'name', c.name, 'recipe_count',"            \
    " (SELECT count(*) FROM recipes WHERE category_id = c.id))"

/*
 * The admin's view of the logins, newest first: the 500 clients seen last and
 * the 300 latest attempts.
 */
#define LOGIN_REPORT_SQL                                                                  \
    "SELECT json_object("                                                                 \
    " 'clients', (SELECT json_group_array(json(client)) FROM ("                          \
    "  SELECT json_object('id', c.id, 'ip', c.ip, 'country', c.country,"                 \
    "   'first_seen', c.first_seen, 'last_seen', c.last_seen,"                           \
    "   'failures', (SELECT count(*) FROM login_attempts a"                              \
    "    WHERE a.client_id = c.id AND a.succeeded = 0),"                                 \
    "   'successes', (SELECT count(*) FROM login_attempts a"                             \
    "    WHERE a.client_id = c.id AND a.succeeded = 1),"                                 \
    "   'last_failure', (SELECT max(a.at) FROM login_attempts a"                         \
    "    WHERE a.client_id = c.id AND a.succeeded = 0)) AS client"                       \
    "  FROM login_clients c ORDER BY c.last_seen DESC, c.id DESC LIMIT 500)),"           \
    " 'attempts', (SELECT json_group_array(json(attempt)) FROM ("                        \
    "  SELECT json_object('id', a.id, 'client_id', a.client_id, 'at', a.at,"             \
    "   'ip', c.ip, 'country', c.country, 'username', a.username,"                       \
    "   'user_agent', a.user_agent, 'succeeded', json(iif(a.succeeded, 'true', 'false')))" \
    "   AS attempt"                                                                       \
    "  FROM login_attempts a JOIN login_clients c ON c.id = a.client_id"                 \
    "  ORDER BY a.at DESC, a.id DESC LIMIT 300)))"

static bool prepare(DB *db, sqlite3_stmt **stmt, Str sql) {
    return sql_prepare(db->conn, stmt, sql, true);
}

/**
 * Prepares the statements for recipes and for the JSON checks they need.
 */
static bool prepare_recipe_statements(DB *db) {
    return prepare(db, &db->is_json_object,
                STR("SELECT CASE WHEN json_valid(?1) THEN json_type(?1) = 'object' ELSE 0 END")) &&
        prepare(db, &db->list_recipes,
                STR("SELECT json_group_array(json(recipe)) FROM ("
                " SELECT " RECIPE_SUMMARY_JSON " AS recipe"
                " FROM recipes r JOIN categories c ON c.id = r.category_id ORDER BY r.id)")) &&
        prepare(db, &db->get_recipe,
                STR("SELECT " RECIPE_JSON
                " FROM recipes r JOIN categories c ON c.id = r.category_id WHERE r.id = ?1")) &&
        prepare(db, &db->get_recipe_by_slug,
                STR("SELECT " RECIPE_JSON
                " FROM recipes r JOIN categories c ON c.id = r.category_id WHERE r.slug = ?1")) &&
        prepare(db, &db->find_recipe_slug, STR("SELECT 1 FROM recipes WHERE slug = ?1")) &&
        prepare(db, &db->insert_recipe,
                STR("INSERT INTO recipes (category_id, slug, name, image, description) VALUES ("
                INTEGER_FIELD("1", "category_id") ", "
                TEXT_FIELD("1", "slug", "NULL") ", "
                TEXT_FIELD("1", "name", "NULL") ", "
                TEXT_FIELD("1", "image", "''") ", "
                TEXT_FIELD("1", "description", "''") ")"
                " RETURNING id")) &&
        prepare(db, &db->update_recipe,
                STR("UPDATE recipes SET"
                " category_id = " INTEGER_FIELD("2", "category_id") ","
                " slug = " TEXT_FIELD("2", "slug", "NULL") ","
                " name = " TEXT_FIELD("2", "name", "NULL") ","
                " image = " TEXT_FIELD("2", "image", "''") ","
                " description = " TEXT_FIELD("2", "description", "''")
                " WHERE id = ?1")) &&
        prepare(db, &db->delete_recipe, STR("DELETE FROM recipes WHERE id = ?1"));
}

/**
 * Prepares the statements for categories and images.
 */
static bool prepare_category_and_image_statements(DB *db) {
    return prepare(db, &db->list_categories,
                STR("SELECT json_group_array(json(category)) FROM ("
                " SELECT " CATEGORY_JSON " AS category"
                " FROM categories c ORDER BY c.name COLLATE NOCASE)")) &&
        prepare(db, &db->get_category,
                STR("SELECT " CATEGORY_JSON " FROM categories c WHERE c.id = ?1")) &&
        prepare(db, &db->insert_category,
                STR("INSERT INTO categories (name) VALUES (trim("
                TEXT_FIELD("1", "name", "NULL") "))"
                " RETURNING id")) &&
        prepare(db, &db->update_category,
                STR("UPDATE categories SET name = trim(" TEXT_FIELD("2", "name", "NULL") ")"
                " WHERE id = ?1")) &&
        prepare(db, &db->delete_category, STR("DELETE FROM categories WHERE id = ?1")) &&
        prepare(db, &db->list_images,
                STR("SELECT json_group_array(json(image)) FROM ("
                " SELECT json_object('id', i.id, 'url', " IMAGE_URL ","
                " 'original_name', i.original_name, 'size', i.size,"
                " 'created_at', i.created_at, 'used', " IMAGE_USE_COUNT ","
                " 'recipes', json(" IMAGE_RECIPES ")) AS image"
                " FROM images i ORDER BY i.id DESC)")) &&
        prepare(db, &db->find_image,
                STR("SELECT i.extension, " IMAGE_USE_COUNT " FROM images i WHERE i.id = ?1")) &&
        prepare(db, &db->insert_image,
                STR("INSERT INTO images (extension, original_name, size) VALUES (?1, ?2, ?3)"
                " RETURNING id")) &&
        prepare(db, &db->delete_image, STR("DELETE FROM images WHERE id = ?1"));
}

/**
 * Prepares the statements for logins and their report.
 */
static bool prepare_login_statements(DB *db) {
    return prepare(db, &db->parse_login,
                STR("SELECT json_extract(?1, '$.username'), json_extract(?1, '$.password')")) &&
        prepare(db, &db->upsert_login_client,
                STR("INSERT INTO login_clients (ip, country, first_seen, last_seen)"
                " VALUES (?1, nullif(?2, ''), ?3, ?3)"
                " ON CONFLICT (ip) DO UPDATE SET last_seen = excluded.last_seen,"
                " country = coalesce(excluded.country, login_clients.country)"
                " RETURNING id")) &&
        prepare(db, &db->insert_login_attempt,
                STR("INSERT INTO login_attempts (client_id, at, username, user_agent, succeeded)"
                " VALUES (?1, ?2, ?3, ?4, ?5)")) &&
        prepare(db, &db->prune_login_attempts, STR("DELETE FROM login_attempts WHERE at < ?1")) &&
        prepare(db, &db->prune_login_clients,
                STR("DELETE FROM login_clients WHERE last_seen < ?1")) &&
        prepare(db, &db->login_report, STR(LOGIN_REPORT_SQL)) &&
        prepare(db, &db->recent_login_failures,
                STR("SELECT c.ip, count(*), max(a.at)"
                " FROM login_attempts a JOIN login_clients c ON c.id = a.client_id"
                " WHERE a.succeeded = 0 AND a.at >= ?1 AND a.at > c.cleared_at"
                // By id, not time: a failure in the same second as a success may come after it.
                " AND a.id > coalesce((SELECT max(s.id) FROM login_attempts s"
                " WHERE s.client_id = c.id AND s.succeeded = 1), 0)"
                " GROUP BY c.id")) &&
        prepare(db, &db->clear_login_client,
                STR("UPDATE login_clients SET cleared_at = ?2 WHERE id = ?1 RETURNING ip"));
}

bool db_open(DB *db, Str path, DB_Storage storage) {
    assert(db != NULL);
    *db = (DB) {
        .conn = NULL,
        .is_json_object = NULL,
        .list_recipes = NULL,
        .get_recipe = NULL,
        .get_recipe_by_slug = NULL,
        .find_recipe_slug = NULL,
        .insert_recipe = NULL,
        .update_recipe = NULL,
        .delete_recipe = NULL,
        .list_categories = NULL,
        .get_category = NULL,
        .insert_category = NULL,
        .update_category = NULL,
        .delete_category = NULL,
        .list_images = NULL,
        .find_image = NULL,
        .insert_image = NULL,
        .delete_image = NULL,
        .parse_login = NULL,
        .upsert_login_client = NULL,
        .insert_login_attempt = NULL,
        .prune_login_attempts = NULL,
        .prune_login_clients = NULL,
        .login_report = NULL,
        .recent_login_failures = NULL,
        .clear_login_client = NULL,
    };
    if (!open_connection(&db->conn, path, storage) ||
        !sql_exec(db->conn, STR("PRAGMA foreign_keys = ON;"), STR("enabling foreign keys"))) {
        db_close(db);
        return false;
    }
    bool const ok = prepare_recipe_statements(db) &&
                    prepare_category_and_image_statements(db) &&
                    prepare_login_statements(db);
    if (!ok) {
        db_close(db);
    }
    return ok;
}

void db_close(DB *db) {
    sqlite3_finalize(db->is_json_object);
    sqlite3_finalize(db->list_recipes);
    sqlite3_finalize(db->get_recipe);
    sqlite3_finalize(db->get_recipe_by_slug);
    sqlite3_finalize(db->find_recipe_slug);
    sqlite3_finalize(db->insert_recipe);
    sqlite3_finalize(db->update_recipe);
    sqlite3_finalize(db->delete_recipe);
    sqlite3_finalize(db->list_categories);
    sqlite3_finalize(db->get_category);
    sqlite3_finalize(db->insert_category);
    sqlite3_finalize(db->update_category);
    sqlite3_finalize(db->delete_category);
    sqlite3_finalize(db->list_images);
    sqlite3_finalize(db->find_image);
    sqlite3_finalize(db->insert_image);
    sqlite3_finalize(db->delete_image);
    sqlite3_finalize(db->parse_login);
    sqlite3_finalize(db->upsert_login_client);
    sqlite3_finalize(db->insert_login_attempt);
    sqlite3_finalize(db->prune_login_attempts);
    sqlite3_finalize(db->prune_login_clients);
    sqlite3_finalize(db->login_report);
    sqlite3_finalize(db->recent_login_failures);
    sqlite3_finalize(db->clear_login_client);
    sqlite3_close(db->conn);
    db->conn = NULL;
}

static DB_Status status_from(int rc) {
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

/*
 * For category writes, a bad name breaks a CHECK or NOT NULL constraint; any
 * other constraint (a duplicate name, or a delete that recipes still restrict,
 * which SQLite may report with more than one extended code) is a conflict.
 */
/*
 * For recipe writes, a slug another recipe has is a conflict. A category that
 * does not exist is invalid input, as is any other failed constraint.
 */
static DB_Status recipe_status_from(int rc) {
    if (rc == SQLITE_CONSTRAINT_UNIQUE) {
        return DB_CONFLICT;
    }
    return status_from(rc);
}

static DB_Status category_status_from(int rc) {
    if ((rc & 0xff) == SQLITE_CONSTRAINT && rc != SQLITE_CONSTRAINT_CHECK &&
        rc != SQLITE_CONSTRAINT_NOTNULL) {
        return DB_CONFLICT;
    }
    return status_from(rc);
}

static void finish(sqlite3_stmt *stmt) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
}

static DB_Status check_json_object(DB *db, Str json) {
    if (json.len > MAX_JSON_SIZE || !sql_bind_str(db->is_json_object, 1, json)) {
        finish(db->is_json_object);
        return DB_INVALID;
    }
    int rc = sqlite3_step(db->is_json_object);
    DB_Status status = DB_ERROR;
    if (rc == SQLITE_ROW) {
        status = DB_INVALID;
        if (sqlite3_column_int(db->is_json_object, 0)) {
            status = DB_OK;
        }
    } else {
        status = status_from(rc);
    }
    finish(db->is_json_object);
    return status;
}

/**
 * Appends the single text column of a one-row query.
 */
static DB_Status append_text_result(sqlite3_stmt *stmt, Str_Builder *out) {
    assert(out != NULL);
    int rc = sqlite3_step(stmt);
    DB_Status status;
    if (rc == SQLITE_ROW) {
        if (str_builder_append(out, sql_column_str(stmt, 0))) {
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

static DB_Status get_json(sqlite3_stmt *stmt, int64_t id, Str_Builder *out) {
    assert(out != NULL);
    sqlite3_bind_int64(stmt, 1, id);
    return append_text_result(stmt, out);
}

/**
 * Runs an INSERT ... RETURNING id with the JSON body bound to ?1.
 */
static DB_Status insert_from_json(DB *db, sqlite3_stmt *stmt, Str json, int64_t *id,
                                  DB_Status (*map_error)(int)) {
    DB_Status status = check_json_object(db, json);
    if (status != DB_OK) {
        return status;
    }
    sql_bind_str(stmt, 1, json);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *id = sqlite3_column_int64(stmt, 0);
        rc = sqlite3_step(stmt);  // Runs the statement to completion.
    }
    if (rc == SQLITE_DONE) {
        status = DB_OK;
    } else {
        status = map_error(rc);
    }
    finish(stmt);
    return status;
}

/**
 * Runs an UPDATE or DELETE whose row id is bound to ?1; no change means not found.
 */
static DB_Status change_row(DB *db, sqlite3_stmt *stmt, DB_Status (*map_error)(int)) {
    int rc = sqlite3_step(stmt);
    DB_Status status;
    if (rc != SQLITE_DONE) {
        status = map_error(rc);
    } else if (sqlite3_changes(db->conn) == 0) {
        status = DB_NOT_FOUND;
    } else {
        status = DB_OK;
    }
    finish(stmt);
    return status;
}

static DB_Status update_from_json(DB *db, sqlite3_stmt *stmt, int64_t id, Str json,
                                  DB_Status (*map_error)(int)) {
    DB_Status status = check_json_object(db, json);
    if (status != DB_OK) {
        return status;
    }
    sqlite3_bind_int64(stmt, 1, id);
    sql_bind_str(stmt, 2, json);
    return change_row(db, stmt, map_error);
}

static DB_Status delete_row(DB *db, sqlite3_stmt *stmt, int64_t id, DB_Status (*map_error)(int)) {
    sqlite3_bind_int64(stmt, 1, id);
    return change_row(db, stmt, map_error);
}

DB_Status db_list_recipes_json(DB *db, Str_Builder *out) {
    assert(out != NULL);
    return append_text_result(db->list_recipes, out);
}

DB_Status db_get_recipe_json(DB *db, int64_t id, Str_Builder *out) {
    assert(out != NULL);
    return get_json(db->get_recipe, id, out);
}

DB_Status db_get_recipe_by_slug_json(DB *db, Str slug, Str_Builder *out) {
    assert(out != NULL);
    if (!sql_bind_str(db->get_recipe_by_slug, 1, slug)) {
        finish(db->get_recipe_by_slug);
        return DB_ERROR;
    }
    return append_text_result(db->get_recipe_by_slug, out);
}

DB_Status db_recipe_slug_exists(DB *db, Str slug, bool *exists) {
    assert(exists != NULL);
    sqlite3_stmt *stmt = db->find_recipe_slug;
    DB_Status status = DB_OK;
    if (!sql_bind_str(stmt, 1, slug)) {
        status = DB_ERROR;
    } else {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW || rc == SQLITE_DONE) {
            *exists = rc == SQLITE_ROW;
        } else {
            status = status_from(rc);
        }
    }
    finish(stmt);
    return status;
}

DB_Status db_create_recipe(DB *db, Str json, int64_t *id) {
    assert(id != NULL);
    return insert_from_json(db, db->insert_recipe, json, id, recipe_status_from);
}

DB_Status db_update_recipe(DB *db, int64_t id, Str json) {
    return update_from_json(db, db->update_recipe, id, json, recipe_status_from);
}

DB_Status db_delete_recipe(DB *db, int64_t id) {
    return delete_row(db, db->delete_recipe, id, status_from);
}

DB_Status db_list_categories_json(DB *db, Str_Builder *out) {
    assert(out != NULL);
    return append_text_result(db->list_categories, out);
}

DB_Status db_get_category_json(DB *db, int64_t id, Str_Builder *out) {
    assert(out != NULL);
    return get_json(db->get_category, id, out);
}

DB_Status db_create_category(DB *db, Str json, int64_t *id) {
    assert(id != NULL);
    return insert_from_json(db, db->insert_category, json, id, category_status_from);
}

DB_Status db_update_category(DB *db, int64_t id, Str json) {
    return update_from_json(db, db->update_category, id, json, category_status_from);
}

DB_Status db_delete_category(DB *db, int64_t id) {
    return delete_row(db, db->delete_category, id, category_status_from);
}

DB_Status db_list_images_json(DB *db, Str_Builder *out) {
    assert(out != NULL);
    return append_text_result(db->list_images, out);
}

DB_Status db_create_image(DB *db, Str extension, Str original_name, int64_t size, int64_t *id) {
    assert(id != NULL);
    sqlite3_stmt *stmt = db->insert_image;
    if (!sql_bind_str(stmt, 1, extension) || !sql_bind_str(stmt, 2, original_name) ||
        sqlite3_bind_int64(stmt, 3, size) != SQLITE_OK) {
        finish(stmt);
        return DB_ERROR;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *id = sqlite3_column_int64(stmt, 0);
        rc = sqlite3_step(stmt);  // Runs the statement to completion.
    }
    DB_Status status = DB_OK;
    if (rc != SQLITE_DONE) {
        status = status_from(rc);
    }
    finish(stmt);
    return status;
}

DB_Status db_delete_image(DB *db, int64_t id, Str_Builder *file_name) {
    assert(file_name != NULL);
    sqlite3_stmt *find = db->find_image;
    sqlite3_bind_int64(find, 1, id);
    int rc = sqlite3_step(find);
    DB_Status status = DB_OK;
    if (rc == SQLITE_DONE) {
        status = DB_NOT_FOUND;
    } else if (rc != SQLITE_ROW) {
        status = status_from(rc);
    } else if (sqlite3_column_int64(find, 1) > 0) {
        status = DB_CONFLICT;
    } else {
        str_builder_append_u64(file_name, (uint64_t)id);
        str_builder_append(file_name, STR("."));
        str_builder_append(file_name, sql_column_str(find, 0));
    }
    finish(find);
    if (status != DB_OK) {
        return status;
    }
    return db_forget_image(db, id);
}

DB_Status db_forget_image(DB *db, int64_t id) {
    return delete_row(db, db->delete_image, id, status_from);
}

static bool append_text_column(sqlite3_stmt *stmt, int column, Str_Builder *out) {
    if (sqlite3_column_type(stmt, column) != SQLITE_TEXT) {
        return false;
    }
    return str_builder_append(out, sql_column_str(stmt, column));
}

DB_Status db_parse_login(DB *db, Str json, Str_Builder *username, Str_Builder *password) {
    assert(username != NULL);
    assert(password != NULL);
    DB_Status status = check_json_object(db, json);
    if (status != DB_OK) {
        return status;
    }
    sql_bind_str(db->parse_login, 1, json);
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

/**
 * Runs a statement that returns no rows, with its parameters already bound.
 */
static int run(sqlite3_stmt *stmt) {
    int const rc = sqlite3_step(stmt);
    finish(stmt);
    return rc;
}

static DB_Status record_login_steps(DB *db, DB_Login_Attempt const *attempt) {
    sqlite3_stmt *client = db->upsert_login_client;
    if (!sql_bind_str(client, 1, attempt->ip) || !sql_bind_str(client, 2, attempt->country) ||
        sqlite3_bind_int64(client, 3, attempt->at) != SQLITE_OK) {
        finish(client);
        return DB_ERROR;
    }
    int rc = sqlite3_step(client);
    int64_t client_id = 0;
    if (rc == SQLITE_ROW) {
        client_id = sqlite3_column_int64(client, 0);
        rc = sqlite3_step(client);  // Runs the statement to completion.
    }
    finish(client);
    if (rc != SQLITE_DONE) {
        return status_from(rc);
    }

    sqlite3_stmt *insert = db->insert_login_attempt;
    int succeeded = 0;
    if (attempt->succeeded) {
        succeeded = 1;
    }
    if (sqlite3_bind_int64(insert, 1, client_id) != SQLITE_OK ||
        sqlite3_bind_int64(insert, 2, attempt->at) != SQLITE_OK ||
        !sql_bind_str(insert, 3, attempt->username) ||
        !sql_bind_str(insert, 4, attempt->user_agent) ||
        sqlite3_bind_int(insert, 5, succeeded) != SQLITE_OK) {
        finish(insert);
        return DB_ERROR;
    }
    rc = run(insert);
    if (rc != SQLITE_DONE) {
        return status_from(rc);
    }

    int64_t const cutoff = attempt->at - LOGIN_RETENTION_SEC;
    sqlite3_bind_int64(db->prune_login_attempts, 1, cutoff);
    rc = run(db->prune_login_attempts);
    if (rc == SQLITE_DONE) {
        sqlite3_bind_int64(db->prune_login_clients, 1, cutoff);
        rc = run(db->prune_login_clients);
    }
    if (rc == SQLITE_DONE) {
        return DB_OK;
    }
    return status_from(rc);
}

DB_Status db_record_login(DB *db, DB_Login_Attempt const *attempt) {
    if (!sql_exec(db->conn, STR("BEGIN IMMEDIATE;"), STR("starting to record a login"))) {
        return DB_BUSY;
    }
    DB_Status const status = record_login_steps(db, attempt);
    if (status == DB_OK && sql_exec(db->conn, STR("COMMIT;"), STR("recording a login"))) {
        return DB_OK;
    }
    sql_exec(db->conn, STR("ROLLBACK;"), STR("undoing a login record"));
    if (status == DB_OK) {
        return DB_ERROR;
    }
    return status;
}

DB_Status db_login_report_json(DB *db, Str_Builder *out) {
    assert(out != NULL);
    return append_text_result(db->login_report, out);
}

DB_Status db_recent_login_failures(DB *db, int64_t since, DB_Failure_Callback each,
                                   void *context) {
    assert(each != NULL);
    sqlite3_stmt *stmt = db->recent_login_failures;
    sqlite3_bind_int64(stmt, 1, since);
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int64_t const failures = sqlite3_column_int64(stmt, 1);
        uint32_t count = (uint32_t)failures;
        if (failures > UINT32_MAX) {
            count = UINT32_MAX;
        }
        each(context, sql_column_str(stmt, 0), count, sqlite3_column_int64(stmt, 2));
    }
    finish(stmt);
    if (rc == SQLITE_DONE) {
        return DB_OK;
    }
    return status_from(rc);
}

DB_Status db_clear_login_client(DB *db, int64_t id, int64_t now, Str_Builder *ip) {
    assert(ip != NULL);
    sqlite3_stmt *stmt = db->clear_login_client;
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, now);
    int rc = sqlite3_step(stmt);
    DB_Status status;
    if (rc == SQLITE_ROW) {
        str_builder_append(ip, sql_column_str(stmt, 0));
        rc = sqlite3_step(stmt);  // Runs the statement to completion.
        if (rc == SQLITE_DONE) {
            status = DB_OK;
        } else {
            status = status_from(rc);
        }
    } else if (rc == SQLITE_DONE) {
        status = DB_NOT_FOUND;
    } else {
        status = status_from(rc);
    }
    finish(stmt);
    return status;
}
