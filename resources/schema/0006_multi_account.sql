-- FirstContact schema migration 0006 — multi-account support.
--
-- Brings the cache from "one user per database" to "N accounts per
-- database, fully partitioned by account_id". Strategy:
--
--   1. Mint an `accounts` row for the existing single-tenant data set.
--      Its UUID becomes the legacy account id stamped onto every row
--      that's already in the cache.
--   2. SQLite cannot ALTER PRIMARY KEY in place. For each per-account
--      table we follow the canonical "create new, copy, drop, rename"
--      dance with foreign keys disabled inside the transaction so the
--      composite-FK rebuild doesn't trip on the temporary intermediate
--      states.
--   3. FTS5 gets a new `account_id UNINDEXED` column and freshly-rebuilt
--      triggers that propagate it.
--   4. `meta` rows are left alone (still a key/value sheet for global
--      settings) and a parallel `account_meta` table is introduced for
--      anything that lives per-account (history_id, last-used-from
--      sender, notification mode, …). The old global `history_id` and
--      `email` rows in `meta` are migrated into `account_meta` rows for
--      the single-account legacy id, then removed from `meta`.
--
-- Reference: see /tmp/claude-0/.../tasks/ad4f90d019eff3bcd.output §A.

-- The `tmpfc_legacy_account_id` deliberately does NOT survive past the
-- migration — it lives only inside this transaction. We pull it out of
-- a one-row helper table so subsequent statements can reference it via
-- a scalar subquery without juggling Qt parameters in the migration
-- runner. Random UUID as a fallback when there's no email row yet
-- (fresh install before first sign-in).
CREATE TABLE _legacy_account_seed (
    account_id TEXT PRIMARY KEY,
    email TEXT
);

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

-- Seed the legacy account. The synthesised UUID is "00000000-0000-4000-8000-000000000001"
-- — a deterministic v4-shaped value so test fixtures don't need RANDOM().
INSERT INTO _legacy_account_seed(account_id, email)
VALUES (
    '00000000-0000-4000-8000-000000000001',
    COALESCE(
        (SELECT value FROM meta WHERE key = 'email' AND value IS NOT NULL AND value != ''),
        'legacy@local'
    )
);

INSERT INTO accounts(id, email, display_name, sort_order, created_at, is_default)
SELECT account_id, email, NULL, 0, strftime('%s','now')*1000, 1
FROM _legacy_account_seed;

-- 2. Per-account meta. Pulls existing global keys that are now per-account
-- (history_id, email) into rows scoped to the legacy account id; the rest
-- of `meta` stays as global key/value.
CREATE TABLE account_meta (
    account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    key        TEXT NOT NULL,
    value      TEXT,
    PRIMARY KEY (account_id, key)
);

INSERT INTO account_meta(account_id, key, value)
SELECT seed.account_id, 'history_id', m.value
FROM _legacy_account_seed seed
JOIN meta m ON m.key = 'history_id' AND m.value IS NOT NULL;

INSERT INTO account_meta(account_id, key, value)
SELECT seed.account_id, 'email', m.value
FROM _legacy_account_seed seed
JOIN meta m ON m.key = 'email' AND m.value IS NOT NULL;

-- Leave global meta keys (schema_version, fts_version, body_html_version)
-- in place — they're not per-account.
DELETE FROM meta WHERE key IN ('history_id', 'email');

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
CREATE TABLE threads_new (
    account_id                  TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    id                          TEXT NOT NULL,
    history_id                  TEXT,
    snippet                     TEXT,
    last_message_internal_date  INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (account_id, id)
);
INSERT INTO threads_new(account_id, id, history_id, snippet,
                        last_message_internal_date)
SELECT (SELECT account_id FROM _legacy_account_seed), id, history_id, snippet,
       last_message_internal_date
FROM threads;
DROP TABLE threads;
ALTER TABLE threads_new RENAME TO threads;
CREATE INDEX idx_threads_account_lastdate
    ON threads(account_id, last_message_internal_date DESC);

-- ---------- labels ----------
-- Note: parent_id is informational only — Gmail's API never returns one,
-- and LabelTreeModel builds the visual hierarchy by splitting label names
-- on '/'. The single-tenant schema (0001_init.sql) declared a FK from
-- parent_id back to labels(id), but it was never actually populated.
-- For multi-account we drop the parent_id FK entirely; resurrecting a
-- self-pointing composite FK here is fragile (the old table on the right
-- of the rename is the v5 single-tenant one, which doesn't carry an
-- account_id column, so the FK rebuild fails with "foreign key mismatch")
-- and the column carries no real referential semantics today.
CREATE TABLE labels_new (
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
INSERT INTO labels_new(account_id, id, name, type, color_bg, color_fg,
                       message_list_visibility, label_list_visibility,
                       parent_id, unread_count, total_count)
SELECT (SELECT account_id FROM _legacy_account_seed), id, name, type,
       color_bg, color_fg, message_list_visibility, label_list_visibility,
       parent_id, unread_count, total_count
FROM labels;
DROP TABLE labels;
ALTER TABLE labels_new RENAME TO labels;
CREATE INDEX idx_labels_account_name ON labels(account_id, name);

-- ---------- messages ----------
CREATE TABLE messages_new (
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
INSERT INTO messages_new(account_id, id, thread_id, history_id, internal_date,
    size_estimate, from_addr, from_name, to_addrs, cc_addrs, bcc_addrs,
    reply_to, subject, snippet, is_unread, is_starred, is_important,
    has_attachment, body_text, body_html, body_html_present, raw_headers,
    fetched_format, bytes_cached, last_accessed_at, snooze_until, created_at)
SELECT (SELECT account_id FROM _legacy_account_seed), id, thread_id,
    history_id, internal_date, size_estimate, from_addr, from_name,
    to_addrs, cc_addrs, bcc_addrs, reply_to, subject, snippet, is_unread,
    is_starred, is_important, has_attachment, body_text, body_html,
    body_html_present, raw_headers, fetched_format, bytes_cached,
    last_accessed_at, snooze_until, created_at
FROM messages;
DROP TABLE messages;
ALTER TABLE messages_new RENAME TO messages;
CREATE INDEX idx_messages_account_thread        ON messages(account_id, thread_id);
CREATE INDEX idx_messages_account_internal_date ON messages(account_id, internal_date DESC);
CREATE INDEX idx_messages_account_unread        ON messages(account_id, is_unread)
    WHERE is_unread = 1;
CREATE INDEX idx_messages_account_snooze
    ON messages(account_id, snooze_until) WHERE snooze_until IS NOT NULL;

-- ---------- message_labels ----------
CREATE TABLE message_labels_new (
    account_id TEXT NOT NULL,
    message_id TEXT NOT NULL,
    label_id   TEXT NOT NULL,
    PRIMARY KEY (account_id, message_id, label_id),
    FOREIGN KEY (account_id, message_id) REFERENCES messages(account_id, id) ON DELETE CASCADE,
    FOREIGN KEY (account_id, label_id)   REFERENCES labels(account_id, id)   ON DELETE CASCADE
);
INSERT INTO message_labels_new(account_id, message_id, label_id)
SELECT (SELECT account_id FROM _legacy_account_seed), message_id, label_id
FROM message_labels;
DROP TABLE message_labels;
ALTER TABLE message_labels_new RENAME TO message_labels;
CREATE INDEX idx_message_labels_account_label
    ON message_labels(account_id, label_id);

-- ---------- attachments ----------
CREATE TABLE attachments_new (
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
INSERT INTO attachments_new(account_id, id, message_id, filename, mime_type, size, local_path)
SELECT (SELECT account_id FROM _legacy_account_seed), id, message_id, filename, mime_type, size, local_path
FROM attachments;
DROP TABLE attachments;
ALTER TABLE attachments_new RENAME TO attachments;
CREATE INDEX idx_attachments_account_message
    ON attachments(account_id, message_id);

-- ---------- drafts ----------
CREATE TABLE drafts_new (
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
INSERT INTO drafts_new(account_id, id, message_id, thread_id,
                       in_reply_to_msg_id, subject, to_addrs, cc_addrs,
                       bcc_addrs, body_text, updated_at, dirty)
SELECT (SELECT account_id FROM _legacy_account_seed), id, message_id,
       thread_id, in_reply_to_msg_id, subject, to_addrs, cc_addrs,
       bcc_addrs, body_text, updated_at, dirty
FROM drafts;
DROP TABLE drafts;
ALTER TABLE drafts_new RENAME TO drafts;
CREATE INDEX idx_drafts_account_updated
    ON drafts(account_id, updated_at DESC);

-- ---------- outbox ----------
CREATE TABLE outbox_new (
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
INSERT INTO outbox_new(id, account_id, state, rfc5322_blob, thread_id,
                       in_reply_to_message_id, created_at, last_attempt_at,
                       attempt_count, next_retry_at, last_error, send_at)
SELECT id, (SELECT account_id FROM _legacy_account_seed), state,
       rfc5322_blob, thread_id, in_reply_to_message_id, created_at,
       last_attempt_at, attempt_count, next_retry_at, last_error, send_at
FROM outbox;
DROP TABLE outbox;
ALTER TABLE outbox_new RENAME TO outbox;
CREATE INDEX idx_outbox_account_due
    ON outbox(account_id, next_retry_at);

-- ---------- pending_ops ----------
CREATE TABLE pending_ops_new (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id  TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    op_type     TEXT NOT NULL,
    message_id  TEXT NOT NULL,
    payload     TEXT NOT NULL,
    created_at  INTEGER NOT NULL,
    attempts    INTEGER NOT NULL DEFAULT 0,
    last_error  TEXT
);
INSERT INTO pending_ops_new(id, account_id, op_type, message_id, payload,
                            created_at, attempts, last_error)
SELECT id, (SELECT account_id FROM _legacy_account_seed), op_type,
       message_id, payload, created_at, attempts, last_error
FROM pending_ops;
DROP TABLE pending_ops;
ALTER TABLE pending_ops_new RENAME TO pending_ops;
CREATE INDEX idx_pending_ops_account
    ON pending_ops(account_id);

-- ---------- messages_fts (rebuild) ----------
DROP TABLE IF EXISTS messages_fts;
CREATE VIRTUAL TABLE messages_fts USING fts5(
    subject, from_text, body, snippet,
    account_id UNINDEXED,
    content='messages',
    content_rowid='rowid',
    tokenize = 'unicode61 remove_diacritics 2'
);

-- Repopulate the FTS index from the rebuilt messages table. This is the
-- first time the FTS table sees the account_id column so we have to do
-- a one-shot rebuild rather than relying on the triggers.
INSERT INTO messages_fts(rowid, subject, from_text, body, snippet, account_id)
SELECT rowid,
       COALESCE(subject, ''),
       COALESCE(from_name, '') || ' ' || COALESCE(from_addr, ''),
       COALESCE(body_text, ''),
       COALESCE(snippet, ''),
       account_id
FROM messages;

-- Re-create the triggers, now propagating account_id.
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

-- 5. Drop the legacy seed helper and bump fts_version (the FTS schema
-- changed shape, so any pre-0006 fts_version is no longer valid).
DROP TABLE _legacy_account_seed;

INSERT INTO meta(key, value) VALUES ('fts_version', '2')
    ON CONFLICT(key) DO UPDATE SET value = '2';
