#include "db.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "buddy.h"
#include "migrate.h"
#include "sql.h"

#define MAX_FILE_PATH 1024

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

/*
 * SQLite's "unix-dotfile" VFS locks a database by creating a directory called
 * "<database>.lock" and removing it afterwards. Creating a directory is atomic
 * on network shares that cannot do POSIX byte-range locks. It also keeps other
 * processes out, such as the old revision while a new one starts.
 */
static char const *vfs_for(Db_Storage storage) {
    // sqlite3_open_v2() takes a C string; NULL selects the default VFS.
    return storage == DB_STORAGE_SHARE ? "unix-dotfile" : NULL;
}

static bool open_connection(sqlite3 **conn, Str path, Db_Storage storage) {
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

bool db_prepare_file(Str path, Db_Storage storage, Str migrations_dir) {
    if (storage == DB_STORAGE_SHARE) {
        clear_stale_lock(path);
    }
    sqlite3 *conn;
    if (!open_connection(&conn, path, storage)) {
        return false;
    }
    // Table rebuilds in migrations need foreign keys off; checked once at the end.
    Str journal_mode = storage == DB_STORAGE_LOCAL ? STR("PRAGMA journal_mode = WAL;")
                                                    : STR("PRAGMA journal_mode = DELETE;");
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
    " 'description', r.description, 'category_id', c.id, 'category', c.name)"

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
    " 'excerpt', " RECIPE_EXCERPT ", 'category_id', c.id, 'category', c.name)"

// Where an image is served from, and how many recipes show it.
#define IMAGE_URL "('/uploads/' || i.id || '.' || i.extension)"
#define IMAGE_USE_COUNT                                                  \
    "(SELECT count(*) FROM recipes"                                      \
    " WHERE image = " IMAGE_URL " OR instr(description, " IMAGE_URL ") > 0)"

#define CATEGORY_JSON                                                    \
    "json_object('id', c.id, 'name', c.name, 'recipe_count',"            \
    " (SELECT count(*) FROM recipes WHERE category_id = c.id))"

static bool prepare(Db *db, sqlite3_stmt **stmt, Str sql) {
    return sql_prepare(db->conn, stmt, sql, true);
}

bool db_open(Db *db, Str path, Db_Storage storage) {
    *db = (Db) {
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
    };
    if (!open_connection(&db->conn, path, storage) || !sql_exec(db->conn, STR("PRAGMA foreign_keys = ON;"), STR("enabling foreign keys"))) {
        db_close(db);
        return false;
    }
    bool ok =
        prepare(db, &db->is_json_object,
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
        prepare(db, &db->delete_recipe, STR("DELETE FROM recipes WHERE id = ?1")) &&
        prepare(db, &db->list_categories,
                STR("SELECT json_group_array(json(category)) FROM ("
                " SELECT " CATEGORY_JSON " AS category"
                " FROM categories c ORDER BY c.name COLLATE NOCASE)")) &&
        prepare(db, &db->get_category,
                STR("SELECT " CATEGORY_JSON " FROM categories c WHERE c.id = ?1")) &&
        prepare(db, &db->insert_category,
                STR("INSERT INTO categories (name) VALUES (trim(" TEXT_FIELD("1", "name", "NULL") "))"
                " RETURNING id")) &&
        prepare(db, &db->update_category,
                STR("UPDATE categories SET name = trim(" TEXT_FIELD("2", "name", "NULL") ")"
                " WHERE id = ?1")) &&
        prepare(db, &db->delete_category, STR("DELETE FROM categories WHERE id = ?1")) &&
        prepare(db, &db->list_images,
                STR("SELECT json_group_array(json(image)) FROM ("
                " SELECT json_object('id', i.id, 'url', " IMAGE_URL ","
                " 'original_name', i.original_name, 'size', i.size,"
                " 'created_at', i.created_at, 'used', " IMAGE_USE_COUNT ") AS image"
                " FROM images i ORDER BY i.id DESC)")) &&
        prepare(db, &db->find_image,
                STR("SELECT i.extension, " IMAGE_USE_COUNT " FROM images i WHERE i.id = ?1")) &&
        prepare(db, &db->insert_image,
                STR("INSERT INTO images (extension, original_name, size) VALUES (?1, ?2, ?3)"
                " RETURNING id")) &&
        prepare(db, &db->delete_image, STR("DELETE FROM images WHERE id = ?1")) &&
        prepare(db, &db->parse_login,
                STR("SELECT json_extract(?1, '$.username'), json_extract(?1, '$.password')"));
    if (!ok) {
        db_close(db);
    }
    return ok;
}

void db_close(Db *db) {
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

/*
 * For category writes, a bad name breaks a CHECK or NOT NULL constraint; any
 * other constraint (a duplicate name, or a delete that recipes still restrict,
 * which SQLite may report with more than one extended code) is a conflict.
 */
/*
 * For recipe writes, a slug another recipe has is a conflict. A category that
 * does not exist is invalid input, as is any other failed constraint.
 */
static Db_Status recipe_status_from(int rc) {
    return rc == SQLITE_CONSTRAINT_UNIQUE ? DB_CONFLICT : status_from(rc);
}

static Db_Status category_status_from(int rc) {
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

static Db_Status check_json_object(Db *db, Str json) {
    if (json.len > 1024 * 1024 || !sql_bind_str(db->is_json_object, 1, json)) {
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

static Db_Status get_json(sqlite3_stmt *stmt, int64_t id, Str_Builder *out) {
    sqlite3_bind_int64(stmt, 1, id);
    return append_text_result(stmt, out);
}

// Runs an INSERT ... RETURNING id with the JSON body bound to ?1.
static Db_Status insert_from_json(Db *db, sqlite3_stmt *stmt, Str json, int64_t *id,
                                  Db_Status (*map_error)(int)) {
    Db_Status status = check_json_object(db, json);
    if (status != DB_OK) {
        return status;
    }
    sql_bind_str(stmt, 1, json);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *id = sqlite3_column_int64(stmt, 0);
        rc = sqlite3_step(stmt);  // Runs the statement to completion.
    }
    status = rc == SQLITE_DONE ? DB_OK : map_error(rc);
    finish(stmt);
    return status;
}

// Runs an UPDATE or DELETE whose row id is bound to ?1; no change means not found.
static Db_Status change_row(Db *db, sqlite3_stmt *stmt, Db_Status (*map_error)(int)) {
    int rc = sqlite3_step(stmt);
    Db_Status status;
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

static Db_Status update_from_json(Db *db, sqlite3_stmt *stmt, int64_t id, Str json,
                                  Db_Status (*map_error)(int)) {
    Db_Status status = check_json_object(db, json);
    if (status != DB_OK) {
        return status;
    }
    sqlite3_bind_int64(stmt, 1, id);
    sql_bind_str(stmt, 2, json);
    return change_row(db, stmt, map_error);
}

static Db_Status delete_row(Db *db, sqlite3_stmt *stmt, int64_t id, Db_Status (*map_error)(int)) {
    sqlite3_bind_int64(stmt, 1, id);
    return change_row(db, stmt, map_error);
}

Db_Status db_list_recipes_json(Db *db, Str_Builder *out) {
    return append_text_result(db->list_recipes, out);
}

Db_Status db_get_recipe_json(Db *db, int64_t id, Str_Builder *out) {
    return get_json(db->get_recipe, id, out);
}

Db_Status db_get_recipe_by_slug_json(Db *db, Str slug, Str_Builder *out) {
    if (!sql_bind_str(db->get_recipe_by_slug, 1, slug)) {
        finish(db->get_recipe_by_slug);
        return DB_ERROR;
    }
    return append_text_result(db->get_recipe_by_slug, out);
}

Db_Status db_recipe_slug_exists(Db *db, Str slug, bool *exists) {
    sqlite3_stmt *stmt = db->find_recipe_slug;
    Db_Status status = DB_OK;
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

Db_Status db_create_recipe(Db *db, Str json, int64_t *id) {
    return insert_from_json(db, db->insert_recipe, json, id, recipe_status_from);
}

Db_Status db_update_recipe(Db *db, int64_t id, Str json) {
    return update_from_json(db, db->update_recipe, id, json, recipe_status_from);
}

Db_Status db_delete_recipe(Db *db, int64_t id) {
    return delete_row(db, db->delete_recipe, id, status_from);
}

Db_Status db_list_categories_json(Db *db, Str_Builder *out) {
    return append_text_result(db->list_categories, out);
}

Db_Status db_get_category_json(Db *db, int64_t id, Str_Builder *out) {
    return get_json(db->get_category, id, out);
}

Db_Status db_create_category(Db *db, Str json, int64_t *id) {
    return insert_from_json(db, db->insert_category, json, id, category_status_from);
}

Db_Status db_update_category(Db *db, int64_t id, Str json) {
    return update_from_json(db, db->update_category, id, json, category_status_from);
}

Db_Status db_delete_category(Db *db, int64_t id) {
    return delete_row(db, db->delete_category, id, category_status_from);
}

Db_Status db_list_images_json(Db *db, Str_Builder *out) {
    return append_text_result(db->list_images, out);
}

Db_Status db_create_image(Db *db, Str extension, Str original_name, int64_t size, int64_t *id) {
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
    Db_Status status = rc == SQLITE_DONE ? DB_OK : status_from(rc);
    finish(stmt);
    return status;
}

Db_Status db_delete_image(Db *db, int64_t id, Str_Builder *file_name) {
    sqlite3_stmt *find = db->find_image;
    sqlite3_bind_int64(find, 1, id);
    int rc = sqlite3_step(find);
    Db_Status status = DB_OK;
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

Db_Status db_forget_image(Db *db, int64_t id) {
    return delete_row(db, db->delete_image, id, status_from);
}

static bool append_text_column(sqlite3_stmt *stmt, int column, Str_Builder *out) {
    if (sqlite3_column_type(stmt, column) != SQLITE_TEXT) {
        return false;
    }
    return str_builder_append(out, sql_column_str(stmt, column));
}

Db_Status db_parse_login(Db *db, Str json, Str_Builder *username, Str_Builder *password) {
    Db_Status status = check_json_object(db, json);
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
