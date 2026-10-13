/**
 * @file db.h
 * @brief Recipe and category storage in SQLite.
 *
 * SQLite is configured to take all of its memory from a fixed buddy heap
 * handed over at startup, so it never calls malloc. Every worker thread owns
 * one connection with its statements prepared up front; connections are never
 * shared between threads.
 *
 * JSON is parsed and produced by SQLite's built-in JSON functions, which also
 * take care of escaping.
 */

#ifndef DB_H
#define DB_H

#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "str.h"

typedef enum {
    DB_OK,
    DB_NOT_FOUND,
    DB_INVALID,    /**< Malformed JSON or a value that breaks the schema's rules. */
    DB_CONFLICT,   /**< A duplicate name, or a category that recipes still use. */
    DB_TOO_LARGE,  /**< The result did not fit in the output buffer. */
    DB_BUSY,       /**< The database stayed locked by another writer. */
    DB_ERROR,
} Db_Status;

/**
 * Where the database file lives. Local disks support SQLite's normal POSIX file
 * locks and write-ahead logging. Network shares (Azure Files over SMB) support
 * neither reliably, so SQLite is told to lock with a ".lock" directory next to
 * the database and to keep a rollback journal.
 */
typedef enum {
    DB_STORAGE_LOCAL,
    DB_STORAGE_SHARE,
} Db_Storage;

typedef struct {
    sqlite3      *conn;
    sqlite3_stmt *is_json_object;
    sqlite3_stmt *list_recipes;
    sqlite3_stmt *get_recipe;
    sqlite3_stmt *insert_recipe;
    sqlite3_stmt *update_recipe;
    sqlite3_stmt *delete_recipe;
    sqlite3_stmt *list_categories;
    sqlite3_stmt *get_category;
    sqlite3_stmt *insert_category;
    sqlite3_stmt *update_category;
    sqlite3_stmt *delete_category;
    sqlite3_stmt *parse_login;
} Db;

/**
 * Hands SQLite its heap and initializes the library. Call once, before any
 * other function here and before starting threads.
 *
 * @param heap_mem Memory for the heap, aligned to 16 bytes.
 * @param heap_size Heap size; BUDDY_MIN_BLOCK times a power of two.
 * @return False on failure, after printing why.
 */
bool db_global_init(void *heap_mem, size_t heap_size);

/**
 * Creates the database file if needed and applies the migrations in
 * migrations_dir that it has not had yet (see migrate.h).
 *
 * @param path Database file.
 * @param storage Kind of storage the file lives on.
 * @param migrations_dir Directory of <number>__<name>.sql migration files.
 * @return False on failure, after printing why.
 */
bool db_prepare_file(char const *path, Db_Storage storage, char const *migrations_dir);

/**
 * Opens a connection for the calling thread and prepares its statements.
 *
 * @param storage Must match what was passed to db_prepare_file().
 * @return False on failure, after printing why.
 */
bool db_open(Db *db, char const *path, Db_Storage storage);

/**
 * Finalizes statements and closes the connection.
 */
void db_close(Db *db);

/**
 * Appends all recipes as a JSON array of
 * {id, name, image, description, category_id, category}.
 */
Db_Status db_list_recipes_json(Db *db, Str_Builder *out);

/**
 * Appends one recipe as a JSON object.
 */
Db_Status db_get_recipe_json(Db *db, int64_t id, Str_Builder *out);

/**
 * Inserts a recipe from a JSON object with name, image, description and
 * category_id. The category must exist.
 *
 * @param id Receives the new recipe's id.
 */
Db_Status db_create_recipe(Db *db, Str json, int64_t *id);

/**
 * Replaces a recipe's fields from a JSON object like db_create_recipe() takes.
 */
Db_Status db_update_recipe(Db *db, int64_t id, Str json);

/**
 * Deletes a recipe.
 */
Db_Status db_delete_recipe(Db *db, int64_t id);

/**
 * Appends all categories, alphabetically, as a JSON array of
 * {id, name, recipe_count}.
 */
Db_Status db_list_categories_json(Db *db, Str_Builder *out);

/**
 * Appends one category as a JSON object.
 */
Db_Status db_get_category_json(Db *db, int64_t id, Str_Builder *out);

/**
 * Inserts a category from a JSON object with a name.
 *
 * @param id Receives the new category's id.
 * @return DB_CONFLICT when the name is already taken (ignoring case).
 */
Db_Status db_create_category(Db *db, Str json, int64_t *id);

/**
 * Renames a category from a JSON object with a name.
 *
 * @return DB_CONFLICT when the name is already taken (ignoring case).
 */
Db_Status db_update_category(Db *db, int64_t id, Str json);

/**
 * Deletes a category.
 *
 * @return DB_CONFLICT while recipes still belong to it.
 */
Db_Status db_delete_category(Db *db, int64_t id);

/**
 * Extracts the string fields username and password from a JSON object.
 *
 * @param username Receives the username.
 * @param password Receives the password.
 */
Db_Status db_parse_login(Db *db, Str json, Str_Builder *username, Str_Builder *password);

#endif
