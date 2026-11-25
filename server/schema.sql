-- One row per open room; a room not announced for TTL seconds (worker.js) is deleted on the next read.
CREATE TABLE IF NOT EXISTS rooms (
    ip      TEXT    NOT NULL,
    port    INTEGER NOT NULL,
    version INTEGER NOT NULL,
    players INTEGER NOT NULL,
    palace  TEXT    NOT NULL,
    level   TEXT    NOT NULL,
    name    TEXT    NOT NULL,
    seen    INTEGER NOT NULL,
    PRIMARY KEY (ip, port)
);

CREATE INDEX IF NOT EXISTS rooms_seen ON rooms (seen);
