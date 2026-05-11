-- FirstContact schema migration 0007 — drop the synthetic legacy
-- accounts seed row and any data still attached to it.
--
-- Migration 0006 (in earlier revisions) minted a deterministic
-- accounts row at UUID 00000000-0000-4000-8000-000000000001 and
-- stamped every existing single-account cache row with that id so
-- the v0–v5 → v6 upgrade preserved data. That migration path has
-- since been removed: 0006 no longer minds legacy data, and pre-0006
-- caches are rejected at startup. Anyone whose database carries the
-- seed id at this point is on a previously-migrated cache; the seed
-- is no longer a real account, so we delete it. The accounts FK
-- chain (ON DELETE CASCADE) tears down every per-account row that
-- was attached to the seed.
--
-- The DELETE is a no-op on every other database — fresh installs
-- (the row was never created), already-clean migrated installs, and
-- caches that have been wiped manually.

DELETE FROM accounts
WHERE id = '00000000-0000-4000-8000-000000000001';
