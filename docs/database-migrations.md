[Documentation index](../README.md)

# Database migrations

The database structure is defined by SQL files in `migrations/`, applied in
order when the server starts:

| File | Contents |
|---|---|
| `0001__create_recipe_tables.sql` | The `categories` and `recipes` tables |
| `0002__seed_initial_recipes.sql` | Starter content: two categories and the original recipes |
| `0003__create_images_table.sql` | The `images` table for uploaded images |
| `0004__allow_longer_recipe_descriptions.sql` | Descriptions up to 50,000 characters, for editor blocks |
| `0005__add_recipe_slugs.sql` | The unique `slug` of each recipe, made from the names of existing recipes |

**Naming:** `<number>__<name>.sql`, for example `0003__add_servings_to_recipes.sql`:
four digits, two underscores, then lowercase letters, digits and underscores.
Other files in the directory are ignored.

**How it works:**

- Each migration runs once, in its own transaction. If it fails, nothing from
  it is kept and the server does not start, so fix the file and restart.
- Applied migrations are recorded in the `schema_migrations` table
  (`version`, `name`, `applied_at`). The database's current version is the
  highest `version` there.
- Several servers starting at once (as during an Azure deployment) are safe:
  one applies the migrations, the others wait and find nothing left to do.

**Changing the schema:** add a new file with the next number. Do not edit,
rename or renumber a file that has been applied anywhere. The server refuses
to start if:

- an applied migration's file was renamed or renumbered;
- a new file has a number below the database's current version (two branches
  both adding `0003`, say): renumber the new file above the current version;
- the database is newer than the newest file (an older server, newer database).

Migration files must not contain `BEGIN` or `COMMIT`. Foreign key enforcement
is off while migrations run, so tables can be rebuilt; the server checks all
foreign keys once they have finished.

Locally, `docker compose -f dev.docker-compose.yml restart httpd` applies new
migrations. On Azure, `deploy` copies `migrations/` into the image, and they
run when the new version starts.

Databases created before `migrations/` existed (schema version 2) are adopted
on first start: `0001` and `0002` are recorded as applied without running.
