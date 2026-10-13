/**
 * @file migrate.h
 * @brief Applies SQL migration files from a directory to a SQLite database.
 *
 * Migration files are named <number>__<name>.sql, for example
 * 0001__create_users_table.sql: four digits, two underscores, then a name of
 * lowercase letters, digits and underscores. They run in number order, each in
 * its own transaction, so a file must not contain BEGIN or COMMIT itself.
 *
 * Applied migrations are recorded in the table schema_migrations (version,
 * name, applied_at), so the database's current version is its highest version.
 * A migration that has been applied is never run again; change the schema by
 * adding a new file rather than editing an old one.
 */

#ifndef __KASIN_MIGRATE_H__
#define __KASIN_MIGRATE_H__

#include <sqlite3.h>
#include <stdbool.h>

#include "str.h"

/**
 * Brings the database up to date with the migration files in a directory.
 *
 * Safe when several processes start at once: each migration is applied under
 * a write lock and skipped if another process got there first.
 *
 * @param conn Open connection, with foreign key enforcement off so migrations
 *             can rebuild tables.
 * @param dir Directory holding the migration files.
 * @return False on failure, after printing why.
 */
bool migrate(sqlite3 *conn, Str dir);

#endif  // __KASIN_MIGRATE_H__
