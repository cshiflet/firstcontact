-- FirstContact schema migration 0003 — store the HTML body next to the
-- plain-text body.
--
-- Phase 2 only persisted body_text plus a body_html_present boolean flag,
-- on the assumption that the HTML view would render directly from a fresh
-- network fetch. Phase 3's "Open in browser" / inline WebEngine paths need
-- the actual HTML on hand for messages opened from cache, so we add the
-- column. Existing rows have NULL until the next sync re-fetches them.

ALTER TABLE messages ADD COLUMN body_html TEXT;

INSERT INTO meta(key, value) VALUES ('body_html_version', '1');
