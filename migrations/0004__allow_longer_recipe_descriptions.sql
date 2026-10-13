-- Descriptions are now made of editor blocks (titles, paragraphs, images,
-- layouts), which take more room than plain text: raise the limit from 2000 to
-- 50000 characters. SQLite cannot change a CHECK constraint in place, so the
-- table is rebuilt.

CREATE TABLE recipes_new (
    id          INTEGER PRIMARY KEY,
    category_id INTEGER NOT NULL REFERENCES categories (id) ON DELETE RESTRICT,
    name        TEXT NOT NULL
                CHECK (typeof(name) = 'text' AND length(trim(name)) BETWEEN 1 AND 200),
    image       TEXT NOT NULL
                CHECK (typeof(image) = 'text' AND length(image) <= 500),
    description TEXT NOT NULL
                CHECK (typeof(description) = 'text' AND length(description) <= 50000)
);

INSERT INTO recipes_new (id, category_id, name, image, description)
SELECT id, category_id, name, image, description FROM recipes;

DROP TABLE recipes;
ALTER TABLE recipes_new RENAME TO recipes;
CREATE INDEX recipes_by_category ON recipes (category_id);
