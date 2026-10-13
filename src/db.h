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

#ifndef __KASIN_DB_H__
#define __KASIN_DB_H__

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
    sqlite3_stmt *get_recipe_by_slug;
    sqlite3_stmt *find_recipe_slug;
    sqlite3_stmt *insert_recipe;
    sqlite3_stmt *update_recipe;
    sqlite3_stmt *delete_recipe;
    sqlite3_stmt *list_categories;
    sqlite3_stmt *get_category;
    sqlite3_stmt *insert_category;
    sqlite3_stmt *update_category;
    sqlite3_stmt *delete_category;
    sqlite3_stmt *list_images;
    sqlite3_stmt *find_image;
    sqlite3_stmt *insert_image;
    sqlite3_stmt *delete_image;
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
bool db_prepare_file(Str path, Db_Storage storage, Str migrations_dir);

/**
 * Opens a connection for the calling thread and prepares its statements.
 *
 * @param storage Must match what was passed to db_prepare_file().
 * @return False on failure, after printing why.
 */
bool db_open(Db *db, Str path, Db_Storage storage);

/**
 * Finalizes statements and closes the connection.
 */
void db_close(Db *db);

/**
 * Appends all recipes as a JSON array of
 * {id, slug, name, image, excerpt, category_id, category}. The excerpt is plain text
 * from the start of the description (its first paragraph block), so the list
 * stays small however long the descriptions are.
 */
Db_Status db_list_recipes_json(Db *db, Str_Builder *out);

/**
 * Appends one recipe as a JSON object.
 */
Db_Status db_get_recipe_json(Db *db, int64_t id, Str_Builder *out);

/**
 * Appends the recipe with the given slug as a JSON object.
 */
Db_Status db_get_recipe_by_slug_json(Db *db, Str slug, Str_Builder *out);

/**
 * Checks whether a recipe has the given slug.
 *
 * @param exists Receives the answer.
 */
Db_Status db_recipe_slug_exists(Db *db, Str slug, bool *exists);

/**
 * Inserts a recipe from a JSON object with slug, name, image, description and
 * category_id. The category must exist.
 *
 * @param id Receives the new recipe's id.
 * @return DB_CONFLICT when another recipe has the slug.
 */
Db_Status db_create_recipe(Db *db, Str json, int64_t *id);

/**
 * Replaces a recipe's fields from a JSON object like db_create_recipe() takes.
 *
 * @return DB_CONFLICT when another recipe has the slug.
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
 * Appends all uploaded images, newest first, as a JSON array of
 * {id, url, original_name, size, created_at, used, recipes}, where used counts
 * the recipes that show the image and recipes lists them as {id, name}.
 */
Db_Status db_list_images_json(Db *db, Str_Builder *out);

/**
 * Records an uploaded image before its file is written.
 *
 * @param extension File extension: jpg, png, gif or webp.
 * @param original_name Name of the file the user uploaded.
 * @param size File size in bytes.
 * @param id Receives the image's id; the file is named <id>.<extension>.
 */
Db_Status db_create_image(Db *db, Str extension, Str original_name, int64_t size, int64_t *id);

/**
 * Deletes an image's record, unless a recipe still shows it.
 *
 * @param file_name Receives the image's file name, for removing the file.
 * @return DB_CONFLICT while a recipe uses the image.
 */
Db_Status db_delete_image(Db *db, int64_t id, Str_Builder *file_name);

/**
 * Deletes an image's record without checking whether it is used; for undoing
 * db_create_image() when the file could not be written.
 */
Db_Status db_forget_image(Db *db, int64_t id);

/**
 * Extracts the string fields username and password from a JSON object.
 *
 * @param username Receives the username.
 * @param password Receives the password.
 */
Db_Status db_parse_login(Db *db, Str json, Str_Builder *username, Str_Builder *password);

#endif  // __KASIN_DB_H__
