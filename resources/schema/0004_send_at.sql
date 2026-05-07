-- Schedule-send support. send_at is a ms-epoch timestamp; rows whose
-- send_at is NULL OR <= now are due. Existing rows default to NULL
-- ("send immediately") so the migration is a no-op for everything in
-- the queue at the time it runs.
ALTER TABLE outbox ADD COLUMN send_at INTEGER;
