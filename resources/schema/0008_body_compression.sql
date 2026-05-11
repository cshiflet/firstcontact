-- FirstContact schema migration 0008 — zstd-with-trained-dictionary
-- body compression.
--
-- Two related changes:
--
-- 1. body_text / body_html become potential BLOB storage. They keep
--    their TEXT column affinity (SQLite is dynamic), but the codec
--    writes compressed payloads as BLOBs when a per-account dictionary
--    is available. A new `body_compression` flag column tells readers
--    how to interpret the bytes:
--       0 = plaintext (legacy / pre-compression rows)
--       1 = zstd_dict_v1, prefixed with BodyCodec magic bytes
--
-- 2. The FTS5 auto-maintenance triggers from 0006 are dropped. Those
--    triggers read NEW.body_text directly to feed the messages_fts
--    index — fine when body_text is plaintext, but broken once it
--    starts carrying compressed binary. MessageRepository now updates
--    messages_fts programmatically from its upsert/delete paths,
--    feeding the plaintext body (which it has in memory at write
--    time) regardless of how the row's body_text ends up persisted.
--    Net effect: FTS indexing semantics are unchanged; the trigger
--    layer just goes away.

-- 1. Drop the trigger-driven FTS maintenance.
DROP TRIGGER IF EXISTS messages_ai;
DROP TRIGGER IF EXISTS messages_au;
DROP TRIGGER IF EXISTS messages_ad;

-- 2. Add the compression flag. Default 0 (plaintext) so existing rows
--    stay readable until the lazy/recompress backfill rewrites them.
ALTER TABLE messages ADD COLUMN body_compression INTEGER NOT NULL DEFAULT 0;

-- 3. Per-account dictionary slot. Trained once when enough bodies are
--    in the cache; cascades on account removal so a dropCache also
--    wipes the dictionary (it's worthless without its corpus).
CREATE TABLE body_compression_dict (
    account_id TEXT    PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    dict       BLOB    NOT NULL,
    version    INTEGER NOT NULL DEFAULT 1,
    created_at INTEGER NOT NULL,
    -- Stats about the corpus the dict was trained on. Surfaced in
    -- Settings → Storage so the user can decide whether to retrain.
    sample_count INTEGER NOT NULL DEFAULT 0,
    sample_bytes INTEGER NOT NULL DEFAULT 0
);
