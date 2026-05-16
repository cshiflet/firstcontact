# Multi-Account Hardening Plan

This note tracks the pre-release cleanup policy for the multi-account branch.
It is intentionally short: the branch is still before its first supported user
release, so the database contract is simpler than it will be after release.

## Pre-Release Database Policy

- Fresh databases are created from the current consolidated schema.
- Fresh databases start with zero rows in `accounts`; the first successful
  sign-in creates the first real account row.
- Old database versions are rejected at startup. Developers and testers should
  run `firstcontact reset-db` to discard the local cache and create a fresh
  database from the consolidated schema.
- Do not add `schema_history` yet. The project should introduce durable schema
  history and forward migrations with the first supported release.
- Do not add compatibility migrations for pre-release database layouts. The
  branch has not promised on-disk compatibility yet, and rejecting old caches is
  less risky than carrying untested upgrade paths.

## Cleanup Checklist

- Keep UI account lists filtered to authorized accounts. Signed-out rows can
  remain for cache management, but they are not active sign-in targets.
- Keep tests explicit when they need an account row; fresh test databases should
  not rely on seeded synthetic accounts.
- Remove stale comments that describe older pre-release account seeding
  behavior.
