-- Uploaded images. The file is stored as <id>.<extension> in the uploads
-- directory and served at /uploads/<id>.<extension>. AUTOINCREMENT keeps ids of
-- deleted images from being reused, so a cached image never changes.

CREATE TABLE images (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    extension     TEXT NOT NULL CHECK (extension IN ('jpg', 'png', 'gif', 'webp')),
    original_name TEXT NOT NULL DEFAULT ''
                  CHECK (typeof(original_name) = 'text' AND length(original_name) <= 255),
    size          INTEGER NOT NULL CHECK (size > 0),
    created_at    TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);
