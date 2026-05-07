-- Snooze support. snooze_until is a ms-epoch timestamp; when set, the
-- message is hidden from INBOX and a background timer re-applies the
-- INBOX label once now >= snooze_until. NULL means "not snoozed".
--
-- Snooze is fully client-side: Gmail's REST API doesn't expose its
-- own SNOOZED state, so the user only sees snoozed-message wake-ups
-- inside FirstContact. The label transition (drop INBOX) on snooze
-- still propagates to Gmail web; the wake-up re-adds INBOX, also
-- propagating.
ALTER TABLE messages ADD COLUMN snooze_until INTEGER;
CREATE INDEX IF NOT EXISTS idx_messages_snooze_until
    ON messages(snooze_until) WHERE snooze_until IS NOT NULL;
