-- Recipes get a slug: the unique, readable part of their public address,
-- /recipes/<slug>. Lowercase letters a-z, digits and single dashes, starting
-- and ending with a letter or digit.
--
-- Existing recipes get a slug made from their name: lowercased, Nordic and a
-- few accented letters turned into plain ones, every other character into a
-- dash. A name that would give a slug already taken gets its id appended.

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
                CHECK (typeof(description) = 'text' AND length(description) <= 50000)
);

INSERT INTO recipes_new (id, category_id, slug, name, image, description)
WITH RECURSIVE
    lowered (id, name) AS (
        SELECT id, lower(replace(replace(replace(replace(replace(replace(replace(replace(replace(replace(
               name, 'ä', 'a'), 'Ä', 'a'), 'ö', 'o'), 'Ö', 'o'), 'å', 'a'), 'Å', 'a'),
               'é', 'e'), 'É', 'e'), 'è', 'e'), 'ü', 'u'))
        FROM recipes
    ),
    -- Walks the name one character at a time, keeping a-z and 0-9.
    walk (id, rest, slug) AS (
        SELECT id, name, '' FROM lowered
        UNION ALL
        SELECT id, substr(rest, 2),
               slug || CASE WHEN substr(rest, 1, 1) GLOB '[a-z0-9]' THEN substr(rest, 1, 1) ELSE '-' END
        FROM walk WHERE rest <> ''
    ),
    tidied (id, slug) AS (
        SELECT id, trim(substr(trim(replace(replace(replace(replace(replace(
               slug, '--', '-'), '--', '-'), '--', '-'), '--', '-'), '--', '-'), '-'), 1, 80), '-')
        FROM walk WHERE rest = ''
    ),
    based (id, slug) AS (
        SELECT id, CASE WHEN slug = '' THEN 'resepti' ELSE slug END FROM tidied
    ),
    numbered (id, slug) AS (
        SELECT id, CASE WHEN row_number() OVER (PARTITION BY slug ORDER BY id) = 1
                        THEN slug ELSE slug || '-' || id END
        FROM based
    )
SELECT r.id, r.category_id, n.slug, r.name, r.image, r.description
FROM recipes r JOIN numbered n ON n.id = r.id;

DROP TABLE recipes;
ALTER TABLE recipes_new RENAME TO recipes;
CREATE INDEX recipes_by_category ON recipes (category_id);
