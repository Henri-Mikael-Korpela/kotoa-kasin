-- Recipes and their categories.
--
-- The CHECK constraints are the single place where field rules live: the API
-- writes JSON values straight into these tables, so invalid input fails here.

CREATE TABLE categories (
    id   INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE COLLATE NOCASE
         CHECK (typeof(name) = 'text' AND length(trim(name)) BETWEEN 1 AND 60)
);

CREATE TABLE recipes (
    id          INTEGER PRIMARY KEY,
    category_id INTEGER NOT NULL REFERENCES categories (id) ON DELETE RESTRICT,
    name        TEXT NOT NULL
                CHECK (typeof(name) = 'text' AND length(trim(name)) BETWEEN 1 AND 200),
    image       TEXT NOT NULL
                CHECK (typeof(image) = 'text' AND length(image) <= 500),
    description TEXT NOT NULL
                CHECK (typeof(description) = 'text' AND length(description) <= 2000)
);

CREATE INDEX recipes_by_category ON recipes (category_id);
