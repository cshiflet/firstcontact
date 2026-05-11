-- FirstContact schema migration 0006 — multi-account schema.
--
-- Establishes the multi-account shape: every per-account cache table
-- carries a composite (account_id, …) primary key, an `accounts` table
-- holds the signed-in identities, and a separate `account_meta` table
-- replaces the per-account-scoped subset of the old global `meta` sheet.
--
-- This migration does NOT carry single-account v0–v5 data forward. The
-- previous draft of 0006 minted a synthetic "legacy seed" accounts row
-- and stamped every existing cache row with its UUID, but that path
-- created more friction than value: the seed row clung around as a
-- fake account that the toolbar / dialogs had to filter out, and the
-- on-the-wire OAuth → first-sync flow was simpler when we knew an
-- accounts row could only come from a real sign-in. Pre-0006 caches
-- are now rejected at startup by `Migrations::run` (see the v < 6
-- guard there); users with an old cache must let the app re-sync from
-- scratch.
--
-- On a fresh install the v1–v5 migrations create empty single-tenant
-- tables; this migration drops and recreates them in multi-tenant
-- shape. The DROP/CREATE/RENAME dance below therefore moves no rows
-- on a fresh install — it's only doing schema work.

-- 1. Accounts table.
CREATE TABLE accounts (
    id              TEXT PRIMARY KEY,             -- stable UUID, NOT the email
    email           TEXT NOT NULL UNIQUE,
    display_name    TEXT,
    color_hint      TEXT,                         -- v3 accent palette key
    sort_order      INTEGER NOT NULL DEFAULT 0,
    created_at      INTEGER NOT NULL,
    last_used_at    INTEGER,
    is_default      INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX idx_accounts_sort       ON accounts(sort_order, email);
CREATE UNIQUE INDEX idx_accounts_email ON accounts(email);

-- 2. Per-account meta. The schema keeps `meta` as a global key/value
-- sheet (schema_version, fts_version, body_html_version) and uses
-- account_meta for anything that varies per account.
CREATE TABLE account_meta (
    account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    key        TEXT NOT NULL,
    value      TEXT,
    PRIMARY KEY (account_id, key)
);

-- 3. FK enforcement is toggled OFF by the runner around this whole
-- migration (PRAGMA foreign_keys must be set outside a transaction;
-- sqlite ignores changes to it inside one). The composite-FK rebuild
-- below would otherwise trip on still-pointing-at-v5-schema child
-- tables during the in-flight states between DROP TABLE and RENAME.
--
-- 4. Drop the FTS triggers up front so messages-table rebuild doesn't
-- fire stale ones.
DROP TRIGGER IF EXISTS messages_ai;
DROP TRIGGER IF EXISTS messages_ad;
DROP TRIGGER IF EXISTS messages_au;

-- ---------- threads ----------
DROP TABLE IF EXISTS threads;
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

-- ---------- labels ----------
-- parent_id is informational only — Gmail's API never returns one,
-- and LabelTreeModel builds the visual hierarchy by splitting label
-- names on '/'. No FK from parent_id back to labels.
DROP TABLE IF EXISTS labels;
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

-- ---------- messages ----------
DROP TABLE IF EXISTS messages;
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
    body_text           TEXT,
    body_html           TEXT,
    body_html_present   INTEGER NOT NULL DEFAULT 0,
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

-- ---------- message_labels ----------
DROP TABLE IF EXISTS message_labels;
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

-- ---------- attachments ----------
DROP TABLE IF EXISTS attachments;
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

-- ---------- drafts ----------
DROP TABLE IF EXISTS drafts;
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

-- ---------- outbox ----------
DROP TABLE IF EXISTS outbox;
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

-- ---------- pending_ops ----------
DROP TABLE IF EXISTS pending_ops;
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

-- ---------- messages_fts ----------
DROP TABLE IF EXISTS messages_fts;
CREATE VIRTUAL TABLE messages_fts USING fts5(
    subject, from_text, body, snippet,
    account_id UNINDEXED,
    content='messages',
    content_rowid='rowid',
    tokenize = 'unicode61 remove_diacritics 2'
);

CREATE TRIGGER messages_ai AFTER INSERT ON messages BEGIN
    INSERT INTO messages_fts(rowid, subject, from_text, body, snippet, account_id)
        VALUES (new.rowid,
                COALESCE(new.subject, ''),
                COALESCE(new.from_name, '') || ' ' || COALESCE(new.from_addr, ''),
                COALESCE(new.body_text, ''),
                COALESCE(new.snippet, ''),
                new.account_id);
END;

CREATE TRIGGER messages_ad AFTER DELETE ON messages BEGIN
    INSERT INTO messages_fts(messages_fts, rowid, subject, from_text, body, snippet, account_id)
        VALUES ('delete', old.rowid,
                COALESCE(old.subject, ''),
                COALESCE(old.from_name, '') || ' ' || COALESCE(old.from_addr, ''),
                COALESCE(old.body_text, ''),
                COALESCE(old.snippet, ''),
                old.account_id);
END;

CREATE TRIGGER messages_au AFTER UPDATE ON messages BEGIN
    INSERT INTO messages_fts(messages_fts, rowid, subject, from_text, body, snippet, account_id)
        VALUES ('delete', old.rowid,
                COALESCE(old.subject, ''),
                COALESCE(old.from_name, '') || ' ' || COALESCE(old.from_addr, ''),
                COALESCE(old.body_text, ''),
                COALESCE(old.snippet, ''),
                old.account_id);
    INSERT INTO messages_fts(rowid, subject, from_text, body, snippet, account_id)
        VALUES (new.rowid,
                COALESCE(new.subject, ''),
                COALESCE(new.from_name, '') || ' ' || COALESCE(new.from_addr, ''),
                COALESCE(new.body_text, ''),
                COALESCE(new.snippet, ''),
                new.account_id);
END;

-- 5. Bump fts_version (the FTS schema changed shape, so any pre-0006
-- fts_version is no longer valid).
INSERT INTO meta(key, value) VALUES ('fts_version', '2')
    ON CONFLICT(key) DO UPDATE SET value = '2';
