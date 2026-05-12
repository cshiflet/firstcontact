-- FirstContact cache database — consolidated schema.
--
-- One file, applied once on a fresh database. No migration walk. If a
-- pre-existing DB doesn't match this schema's version, Migrations::run
-- aborts with a fatal error directing the user to `firstcontact
-- reset-db`. This is a prototype; there are no other users, so we
-- trade migration machinery for simplicity.
--
-- Bump kSchemaVersion in Migrations.cpp when this file changes
-- structurally. Existing local DBs at any other version will be
-- rejected.

-- ---------------------------------------------------------------------
-- meta
-- ---------------------------------------------------------------------
CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT
);

-- ---------------------------------------------------------------------
-- accounts (signed-in Gmail accounts)
-- ---------------------------------------------------------------------
CREATE TABLE accounts (
    id              TEXT PRIMARY KEY,             -- stable UUID, NOT the email
    email           TEXT NOT NULL UNIQUE,
    display_name    TEXT,
    color_hint      TEXT,
    sort_order      INTEGER NOT NULL DEFAULT 0,
    created_at      INTEGER NOT NULL,
    last_used_at    INTEGER,
    is_default      INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX idx_accounts_sort         ON accounts(sort_order, email);
CREATE UNIQUE INDEX idx_accounts_email ON accounts(email);

-- Per-account scratch state (sync cursors, crawl-exhausted flags, etc.).
CREATE TABLE account_meta (
    account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    key        TEXT NOT NULL,
    value      TEXT,
    PRIMARY KEY (account_id, key)
);

-- ---------------------------------------------------------------------
-- threads
-- ---------------------------------------------------------------------
CREATE TABLE threads (
    account_id                  TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    id                          TEXT NOT NULL,
    history_id                  TEXT,
    snippet                     TEXT,
    last_message_internal_date  INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (account_id, id)
);
CREATE INDEX idx_threads_account_lastdate
    ON threads(account_id, last_message_internal_date DESC);

-- ---------------------------------------------------------------------
-- labels (Gmail "labels" = folders + tags)
-- ---------------------------------------------------------------------
CREATE TABLE labels (
    account_id               TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    id                       TEXT NOT NULL,
    name                     TEXT NOT NULL,
    type                     TEXT NOT NULL,
    color_bg                 TEXT,
    color_fg                 TEXT,
    message_list_visibility  TEXT,
    label_list_visibility    TEXT,
    parent_id                TEXT,
    unread_count             INTEGER NOT NULL DEFAULT 0,
    total_count              INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (account_id, id)
);
CREATE INDEX idx_labels_account_name ON labels(account_id, name);

-- ---------------------------------------------------------------------
-- messages
-- ---------------------------------------------------------------------
CREATE TABLE messages (
    account_id          TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    id                  TEXT NOT NULL,
    thread_id           TEXT NOT NULL,
    history_id          TEXT,
    internal_date       INTEGER NOT NULL,
    size_estimate       INTEGER,
    from_addr           TEXT,
    from_name           TEXT,
    to_addrs            TEXT,
    cc_addrs            TEXT,
    bcc_addrs           TEXT,
    reply_to            TEXT,
    subject             TEXT,
    snippet             TEXT,
    is_unread           INTEGER NOT NULL DEFAULT 0,
    is_starred          INTEGER NOT NULL DEFAULT 0,
    is_important        INTEGER NOT NULL DEFAULT 0,
    has_attachment      INTEGER NOT NULL DEFAULT 0,
    -- body_text / body_html keep TEXT affinity but may hold compressed
    -- BLOB bytes (SQLite is dynamic). body_compression interprets them:
    --   0 = plaintext
    --   1 = zstd_dict_v1 (BodyCodec magic-prefixed; see body_compression_dict)
    body_text           TEXT,
    body_html           TEXT,
    body_html_present   INTEGER NOT NULL DEFAULT 0,
    body_compression    INTEGER NOT NULL DEFAULT 0,
    raw_headers         TEXT,
    fetched_format      TEXT,
    bytes_cached        INTEGER NOT NULL DEFAULT 0,
    last_accessed_at    INTEGER,
    snooze_until        INTEGER,
    created_at          INTEGER NOT NULL,
    PRIMARY KEY (account_id, id),
    FOREIGN KEY (account_id, thread_id)
        REFERENCES threads(account_id, id) ON DELETE CASCADE
);
CREATE INDEX idx_messages_account_thread        ON messages(account_id, thread_id);
CREATE INDEX idx_messages_account_internal_date ON messages(account_id, internal_date DESC);
CREATE INDEX idx_messages_account_unread        ON messages(account_id, is_unread)
    WHERE is_unread = 1;
CREATE INDEX idx_messages_account_snooze
    ON messages(account_id, snooze_until) WHERE snooze_until IS NOT NULL;

CREATE TABLE message_labels (
    account_id TEXT NOT NULL,
    message_id TEXT NOT NULL,
    label_id   TEXT NOT NULL,
    PRIMARY KEY (account_id, message_id, label_id),
    FOREIGN KEY (account_id, message_id) REFERENCES messages(account_id, id) ON DELETE CASCADE,
    FOREIGN KEY (account_id, label_id)   REFERENCES labels(account_id, id)   ON DELETE CASCADE
);
CREATE INDEX idx_message_labels_account_label
    ON message_labels(account_id, label_id);

CREATE TABLE attachments (
    account_id  TEXT NOT NULL,
    id          TEXT NOT NULL,
    message_id  TEXT NOT NULL,
    filename    TEXT,
    mime_type   TEXT,
    size        INTEGER,
    local_path  TEXT,
    PRIMARY KEY (account_id, id),
    FOREIGN KEY (account_id, message_id) REFERENCES messages(account_id, id) ON DELETE CASCADE
);
CREATE INDEX idx_attachments_account_message
    ON attachments(account_id, message_id);

-- ---------------------------------------------------------------------
-- drafts
-- ---------------------------------------------------------------------
CREATE TABLE drafts (
    account_id           TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    id                   TEXT NOT NULL,
    message_id           TEXT,
    thread_id            TEXT,
    in_reply_to_msg_id   TEXT,
    subject              TEXT,
    to_addrs             TEXT,
    cc_addrs             TEXT,
    bcc_addrs            TEXT,
    body_text            TEXT,
    updated_at           INTEGER NOT NULL,
    dirty                INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (account_id, id)
);
CREATE INDEX idx_drafts_account_updated
    ON drafts(account_id, updated_at DESC);

-- ---------------------------------------------------------------------
-- outbox  (pending sends — RFC 5322 blobs queued for the OutboxWorker)
-- ---------------------------------------------------------------------
CREATE TABLE outbox (
    id                       INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id               TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    state                    TEXT NOT NULL,
    rfc5322_blob             BLOB NOT NULL,
    thread_id                TEXT,
    in_reply_to_message_id   TEXT,
    created_at               INTEGER NOT NULL,
    last_attempt_at          INTEGER,
    attempt_count            INTEGER NOT NULL DEFAULT 0,
    next_retry_at            INTEGER,
    last_error               TEXT,
    send_at                  INTEGER
);
CREATE INDEX idx_outbox_account_due
    ON outbox(account_id, next_retry_at);

-- ---------------------------------------------------------------------
-- pending_ops  (deferred label flips / mark-read / etc. for the PendingOpsWorker)
-- ---------------------------------------------------------------------
CREATE TABLE pending_ops (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id  TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    op_type     TEXT NOT NULL,
    message_id  TEXT NOT NULL,
    payload     TEXT NOT NULL,
    created_at  INTEGER NOT NULL,
    attempts    INTEGER NOT NULL DEFAULT 0,
    last_error  TEXT
);
CREATE INDEX idx_pending_ops_account
    ON pending_ops(account_id);

-- ---------------------------------------------------------------------
-- messages_fts  (full-text search shadow of messages.subject/from/body/snippet)
--
-- Triggers from earlier schema iterations are intentionally absent —
-- they can't read compressed body_text payloads. MessageRepository now
-- maintains messages_fts programmatically from its upsert/delete paths
-- with plaintext content held in memory at write time.
-- ---------------------------------------------------------------------
CREATE VIRTUAL TABLE messages_fts USING fts5(
    subject, from_text, body, snippet,
    account_id UNINDEXED,
    content='messages',
    content_rowid='rowid',
    tokenize = 'unicode61 remove_diacritics 2'
);

-- ---------------------------------------------------------------------
-- body_compression_dict  (per-account zstd dictionary)
-- ---------------------------------------------------------------------
CREATE TABLE body_compression_dict (
    account_id   TEXT    PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    dict         BLOB    NOT NULL,
    version      INTEGER NOT NULL DEFAULT 1,
    created_at   INTEGER NOT NULL,
    sample_count INTEGER NOT NULL DEFAULT 0,
    sample_bytes INTEGER NOT NULL DEFAULT 0
);
