-- Recipes get a creation time and a time of the last edit, both UTC in the
-- same format as images.created_at (2026-01-31T12:00:00Z).
--
-- The database keeps them: created_at is filled in when a recipe is inserted,
-- and a trigger sets updated_at on every update, so the API never writes them.
--
-- Existing recipes get the time of this migration for both, as their real
-- times are not known.

CREATE TABLE recipes_new (
    id          INTEGER PRIMARY KEY,
    category_id INTEGER NOT NULL REFERENCES categories (id) ON DELETE RESTRICT,
    slug        TEXT NOT NULL UNIQUE
                CHECK (typeof(slug) = 'text' AND length(slug) BETWEEN 1 AND 100
                       AND slug GLOB '[a-z0-9]*' AND slug GLOB '*[a-z0-9]'
                       AND slug NOT GLOB '*[^a-z0-9-]*' AND slug NOT GLOB '*--*'),
    name        TEXT NOT NULL
                CHECK (typeof(name) = 'text' AND length(trim(name)) BETWEEN 1 AND 200),
    image       TEXT NOT NULL
                CHECK (typeof(image) = 'text' AND length(image) <= 500),
    description TEXT NOT NULL
                CHECK (typeof(description) = 'text' AND length(description) <= 50000),
    created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    updated_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

INSERT INTO recipes_new (id, category_id, slug, name, image, description)
SELECT id, category_id, slug, name, image, description FROM recipes;

DROP TABLE recipes;
ALTER TABLE recipes_new RENAME TO recipes;
CREATE INDEX recipes_by_category ON recipes (category_id);

-- The WHEN clause keeps the trigger from firing for its own update.
CREATE TRIGGER recipes_touch_updated_at AFTER UPDATE ON recipes
WHEN NEW.updated_at IS OLD.updated_at
BEGIN
    UPDATE recipes SET updated_at = strftime('%Y-%m-%dT%H:%M:%SZ', 'now') WHERE id = NEW.id;
END;
