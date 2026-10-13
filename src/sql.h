/**
 * @file sql.h
 * @brief SQLite helpers that take and return Str instead of C strings.
 */

#ifndef __KASIN_SQL_H__
#define __KASIN_SQL_H__

#include <sqlite3.h>
#include <stdbool.h>

#include "str.h"

/**
 * Runs one or more semicolon-separated statements, like sqlite3_exec() but
 * with SQL that does not need a NUL terminator.
 *
 * @param conn Open connection.
 * @param sql The statements.
 * @param context Names what is being run; printed with the error on failure.
 * @return False on failure, after printing why to stderr.
 */
bool sql_exec(sqlite3 *conn, Str sql, Str context);

/**
 * Prepares one statement for repeated use.
 *
 * @param persistent True for statements kept for the life of the connection.
 * @return False on failure, after printing why to stderr.
 */
bool sql_prepare(sqlite3 *conn, sqlite3_stmt **stmt, Str sql, bool persistent);

/**
 * Binds text to a parameter; SQLite keeps its own copy.
 *
 * @param index Parameter number, starting at 1.
 * @return False on failure.
 */
bool sql_bind_str(sqlite3_stmt *stmt, int index, Str value);

/**
 * Returns a text column of the current row. The bytes belong to SQLite and stay
 * valid only until the statement is stepped or reset.
 *
 * @param column Column number, starting at 0.
 * @return The text; empty for NULL.
 */
Str sql_column_str(sqlite3_stmt *stmt, int column);

#endif  // __KASIN_SQL_H__
