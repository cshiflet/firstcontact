# Integration tests

Multi-component round-trips against real Qt event loops, real SQLite, real
loopback HTTP. Slower than the unit tests but catch regressions that span
parser → repository → query, or auth-URL construction across `OAuthClient`
+ `QOAuthHttpServerReplyHandler`.

## What's covered

| Test | Exercises |
|---|---|
| `test_sync_pipeline` | Gmail JSON → `MessageParser::parse` → `MessageRepository::upsert` → `listByLabel`. Catches schema/parser drift, ordering, idempotency, `unreadOnly` filter, label-edge round-trip. |
| `test_oauth_flow` | `OAuthClient::authorize()` URL conformance: PKCE S256, 43-char base64url challenge, scope set, loopback redirect, state. Plus the unconfigured-client failure path. |

## Harness conventions

- Tests open their own SQLite cache in `setTestModeEnabled` mode (deletes any prior
  `cache.db` first — `QStandardPaths` redirects but does not wipe).
- The cache `labels` table is empty at fresh-init; tests that need label-edge
  hydration call a local `seedSystemLabels()` to insert `INBOX` / `UNREAD` /
  `STARRED` / `CATEGORY_PERSONAL`.
- `QSettings` is seeded explicitly in `initTestCase` for tests that depend on
  `ClientConfig` (OAuth client_id/client_secret).
- Use `QSignalSpy::wait(ms)` over busy loops — the OAuth flow emits
  `browserAuthRequested` from inside `authorize()` after the loopback
  handler binds.

## Scenarios still to add

- **End-to-end PKCE.** Spin up a `QHttpServer` fake of `accounts.google.com` +
  `oauth2.googleapis.com/token`. Drive a full code-grant flow. Verify
  `TokenStore::save` lands valid tokens and `isAuthorized()` returns true.
- **State mismatch.** Feed a callback with `state` mismatched against the
  stored value; verify `failed()` fires and tokens stay unset.
- **401 mid-session refresh.** With tokens loaded, intercept a Gmail API
  call to return 401, verify silent refresh → re-attempt → success.
- **Revoke.** `signOut()` must hit the revoke endpoint and clear keychain.
- **History sync.** `historyId` advance + delta apply against `MessageRepository`.
- **Outbox round-trip.** Queue, drain, retry-on-failure, success → message
  appears in cache.
- **FTS5 search.** Replay a fixture set, run `searchFts` with the operator
  set we mirror from Gmail web (`from:`, `subject:`, `has:attachment`, …).

The harness pattern (temp `QTemporaryDir`, deterministic seed, scripted
loopback servers) generalises to all of them. New tests follow the same
init-test-case skeleton.
