-- FirstContact schema migration 0001 — initial cache layout.
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT
);

CREATE TABLE labels (
    id                       TEXT PRIMARY KEY,
    name                     TEXT NOT NULL,
    type                     TEXT NOT NULL,            -- 'system' | 'user'
    color_bg                 TEXT,
    color_fg                 TEXT,
    message_list_visibility  TEXT,
    label_list_visibility    TEXT,
    parent_id                TEXT REFERENCES labels(id),
    unread_count             INTEGER NOT NULL DEFAULT 0,
    total_count              INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE threads (
    id                          TEXT PRIMARY KEY,
    history_id                  TEXT,
    snippet                     TEXT,
    last_message_internal_date  INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE messages (
    id                  TEXT PRIMARY KEY,
    thread_id           TEXT NOT NULL REFERENCES threads(id) ON DELETE CASCADE,
    history_id          TEXT,
    internal_date       INTEGER NOT NULL,
    size_estimate       INTEGER,
    from_addr           TEXT,
    from_name           TEXT,
    to_addrs            TEXT,            -- JSON array
    cc_addrs            TEXT,            -- JSON array
    bcc_addrs           TEXT,            -- JSON array
    reply_to            TEXT,
    subject             TEXT,
    snippet             TEXT,
    is_unread           INTEGER NOT NULL DEFAULT 0,
    is_starred          INTEGER NOT NULL DEFAULT 0,
    is_important        INTEGER NOT NULL DEFAULT 0,
    has_attachment      INTEGER NOT NULL DEFAULT 0,
    body_text           TEXT,
    body_html_present   INTEGER NOT NULL DEFAULT 0,
    raw_headers         TEXT,            -- JSON map
    fetched_format      TEXT,            -- 'metadata' | 'full'
    bytes_cached        INTEGER NOT NULL DEFAULT 0,
    last_accessed_at    INTEGER,
    created_at          INTEGER NOT NULL
);
CREATE INDEX idx_messages_thread        ON messages(thread_id);
CREATE INDEX idx_messages_internal_date ON messages(internal_date DESC);
CREATE INDEX idx_messages_unread        ON messages(is_unread) WHERE is_unread = 1;

CREATE TABLE message_labels (
    message_id TEXT NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    label_id   TEXT NOT NULL REFERENCES labels(id)   ON DELETE CASCADE,
    PRIMARY KEY (message_id, label_id)
);
CREATE INDEX idx_message_labels_label ON message_labels(label_id);

CREATE TABLE attachments (
    id          TEXT PRIMARY KEY,        -- Gmail attachmentId
    message_id  TEXT NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    filename    TEXT,
    mime_type   TEXT,
    size        INTEGER,
    local_path  TEXT
);

CREATE TABLE drafts (
    id                   TEXT PRIMARY KEY,        -- Gmail draftId (or local "tmp-…" before first sync)
    message_id           TEXT,                    -- Gmail messageId once persisted
    thread_id            TEXT,
    in_reply_to_msg_id   TEXT,
    subject              TEXT,
    to_addrs             TEXT,
    cc_addrs             TEXT,
    bcc_addrs            TEXT,
    body_text            TEXT,
    updated_at           INTEGER NOT NULL,
    dirty                INTEGER NOT NULL DEFAULT 0   -- needs push to Gmail
);

CREATE TABLE outbox (
    id                       INTEGER PRIMARY KEY AUTOINCREMENT,
    state                    TEXT NOT NULL,        -- 'queued' | 'sending' | 'sent' | 'failed'
    rfc5322_blob             BLOB NOT NULL,
    thread_id                TEXT,
    in_reply_to_message_id   TEXT,
    created_at               INTEGER NOT NULL,
    last_attempt_at          INTEGER,
    attempt_count            INTEGER NOT NULL DEFAULT 0,
    next_retry_at            INTEGER,
    last_error               TEXT
);

CREATE TABLE pending_ops (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    op_type     TEXT NOT NULL,            -- 'modify' | 'trash' | 'untrash'
    message_id  TEXT NOT NULL,
    payload     TEXT NOT NULL,            -- JSON: {addLabelIds:[…], removeLabelIds:[…]}
    created_at  INTEGER NOT NULL,
    attempts    INTEGER NOT NULL DEFAULT 0,
    last_error  TEXT
);

INSERT INTO meta(key, value) VALUES ('schema_version', '1');
