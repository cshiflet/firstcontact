-- FirstContact schema migration 0002 — FTS5 index over messages.
CREATE VIRTUAL TABLE messages_fts USING fts5(
    subject, from_text, body, snippet,
    content='messages',
    content_rowid='rowid',
    tokenize = 'unicode61 remove_diacritics 2'
);

CREATE TRIGGER messages_ai AFTER INSERT ON messages BEGIN
    INSERT INTO messages_fts(rowid, subject, from_text, body, snippet)
        VALUES (new.rowid,
                COALESCE(new.subject, ''),
                COALESCE(new.from_name, '') || ' ' || COALESCE(new.from_addr, ''),
                COALESCE(new.body_text, ''),
                COALESCE(new.snippet, ''));
END;

CREATE TRIGGER messages_ad AFTER DELETE ON messages BEGIN
    INSERT INTO messages_fts(messages_fts, rowid, subject, from_text, body, snippet)
        VALUES ('delete', old.rowid,
                COALESCE(old.subject, ''),
                COALESCE(old.from_name, '') || ' ' || COALESCE(old.from_addr, ''),
                COALESCE(old.body_text, ''),
                COALESCE(old.snippet, ''));
END;

CREATE TRIGGER messages_au AFTER UPDATE ON messages BEGIN
    INSERT INTO messages_fts(messages_fts, rowid, subject, from_text, body, snippet)
        VALUES ('delete', old.rowid,
                COALESCE(old.subject, ''),
                COALESCE(old.from_name, '') || ' ' || COALESCE(old.from_addr, ''),
                COALESCE(old.body_text, ''),
                COALESCE(old.snippet, ''));
    INSERT INTO messages_fts(rowid, subject, from_text, body, snippet)
        VALUES (new.rowid,
                COALESCE(new.subject, ''),
                COALESCE(new.from_name, '') || ' ' || COALESCE(new.from_addr, ''),
                COALESCE(new.body_text, ''),
                COALESCE(new.snippet, ''));
END;

INSERT INTO meta(key, value) VALUES ('fts_version', '1');
