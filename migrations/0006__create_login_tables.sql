-- Login attempts, for brute-force protection and the admin's "Kirjautumiset" view.
--
-- A client is identified by its IP address: one row per address that has
-- tried to log in, with the country it was in when last seen (from the
-- server's offline DB-IP data; NULL when unknown, such as for private
-- networks). Every password check is recorded against its client, with the
-- user name tried and the browser it came from.
--
-- Rows older than 90 days are removed as new attempts are recorded.

CREATE TABLE login_clients (
    id          INTEGER PRIMARY KEY,
    ip          TEXT    NOT NULL UNIQUE
                CHECK (typeof(ip) = 'text' AND length(ip) BETWEEN 2 AND 45),
    country     TEXT    CHECK (country IS NULL OR (typeof(country) = 'text' AND length(country) = 2)),
    first_seen  INTEGER NOT NULL,  -- Unix seconds.
    last_seen   INTEGER NOT NULL,
    -- When an admin last lifted the address's block: earlier failures no
    -- longer count towards a wait or ban (also after a restart).
    cleared_at  INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE login_attempts (
    id          INTEGER PRIMARY KEY,
    client_id   INTEGER NOT NULL REFERENCES login_clients (id) ON DELETE CASCADE,
    at          INTEGER NOT NULL,  -- Unix seconds.
    username    TEXT    NOT NULL CHECK (typeof(username) = 'text' AND length(username) <= 100),
    user_agent  TEXT    NOT NULL CHECK (typeof(user_agent) = 'text' AND length(user_agent) <= 300),
    succeeded   INTEGER NOT NULL CHECK (succeeded IN (0, 1))
);

CREATE INDEX login_attempts_by_time ON login_attempts (at);
CREATE INDEX login_attempts_by_client ON login_attempts (client_id, at);
CREATE INDEX login_clients_by_last_seen ON login_clients (last_seen);
