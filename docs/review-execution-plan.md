# Review Execution Plan

This plan turns the May 2026 code review findings into an execution sequence.
The goal is to fix correctness issues first, then align tests, docs, and setup
with the current implementation.

## Relationship to the Scheduler Plan

This is the near-term stabilization plan. It should be executed before the
larger scheduler migration in [gmail-api-scheduler-plan.md](gmail-api-scheduler-plan.md).

Several findings point at the same long-term issue: Gmail API work is currently
spread across `SyncService`, `OutboxWorker`, `PendingOpsWorker`, `DraftSync`,
`MainWindow`, `RestClient`, and `OAuthClient`, so retry, ordering, token refresh,
rate-limit, and offline behavior live in several places. The scheduler plan is
the consolidation path for that architecture.

Do not block these fixes on the scheduler. The immediate sequence is:

- fix the Qt 6.10 REST canary so tests reflect the current transport behavior;
- fix forced refresh on 401 in the existing `RestClient` / `OAuthClient` path;
- make incremental sync correctly apply Gmail history deltas and deletes;
- clean up docs, package setup, and stale threading comments.

After those land, use the scheduler plan to migrate API dispatch one caller at a
time without changing user-visible behavior.

## 1. Update the Qt Network Canary

`test_rest_client::qnamTransparentlyRetriesPostOnTransportFailure_documented`
documents older Qt 6.4 behavior where `QNetworkAccessManager` transparently
retried POST after a transport drop before any HTTP response bytes.

On Qt 6.10.3, the observed behavior is better for FirstContact: the POST
transport failure is surfaced instead of silently replayed. Update the test to
expect the current behavior:

- `POST` transport failure returns `ApiErrorKind::Network`.
- no application-layer retry occurs.
- the local test server sees one request, not two.
- the historical Qt 6.4 behavior remains documented in a comment.

## 2. Force Refresh on HTTP 401

`RestClient` currently retries once after a 401 by calling the same token getter
again. In production that getter is `OAuthClient::accessTokenBlocking()`, which
only refreshes when the local expiry window says the token is near expiry.

This misses cases where Gmail returns 401 for a revoked, invalidated, or
prematurely expired access token whose local expiry is still in the future.

Implementation:

- Add an explicit forced-refresh API to `OAuthClient`.
- Thread that capability into `RestClient` without weakening the test-friendly
  injected-token constructor.
- On first 401, force refresh before retrying.
- Add/adjust tests for a stale-but-not-expired token that must be refreshed
  after 401.

## 3. Handle Gmail History Label Deltas

`GmailClient::listHistory` requests `labelAdded` and `labelRemoved`, but the
parser and sync path currently only act on message additions/deletions.

Implementation:

- Extend the history data model to carry label additions and removals per
  message.
- Parse Gmail `labelsAdded` and `labelsRemoved` entries.
- During incremental sync, update cached message label edges and derived flags
  such as unread, starred, and important.
- Recompute label counts after applying deltas.
- Add tests covering read/unread, archive, star, and label changes made outside
  FirstContact.

## 4. Apply Remote Deletes

`SyncService` collects `messagesDeleted` but does not remove the local cache
rows or label edges. Deleted Gmail messages can remain visible locally until a
later cleanup path.

Decision: hard-delete cached message rows and their dependent rows for now.
This is simpler than tombstoning and matches user expectations for a Gmail
client cache.

Implementation:

- Add repository support for deleting messages by account/message id.
- Apply `messagesDeleted` during incremental sync.
- Ensure FTS entries, label edges, attachments, and thread state remain
  consistent.
- Add tests proving deleted messages disappear from label listings and search.

## 5. Align Threading Comments With Reality

`AccountContext` currently states that per-account objects live on the UI
thread. Several other comments still describe a dedicated per-account sync
thread.

Implementation:

- Update comments in `MainWindow`, `SyncService`, `OutboxWorker`, and
  `PendingOpsWorker` to reflect the current UI-thread object model.
- Keep the existing queued-invocation wrappers only where they still have value
  for event-loop ordering or future-proofing.
- Do not reintroduce worker threads as part of this cleanup.

## 6. Fix Docs and Fedora Setup Drift

The README says no OAuth client secret is needed, but the current setup wizard
and OAuth token exchange require one. Fedora setup dependencies also need to
match the verified build/test path.

Implementation:

- Update first-run setup docs to say both Client ID and Client Secret are
  currently required.
- Update the Fedora setup package list to include:
  - `libasan`
  - `libubsan`
  - `pkgconf-pkg-config`
  - `xdg-utils`
- Keep package names aligned with Fedora’s QtKeychain package
  (`qtkeychain-qt6-devel`).

## 7. Verify Through Makefile

Use the Makefile as the supported developer entry point.

Non-sanitized:

```bash
HOME=/tmp/firstcontact-test-home \
XDG_DATA_HOME=/tmp/firstcontact-test-data \
XDG_CONFIG_HOME=/tmp/firstcontact-test-config \
QT_QPA_PLATFORM=offscreen \
make test BUILD_DIR=build/make-nosan SANITIZERS=OFF
```

Sanitized:

```bash
HOME=/tmp/firstcontact-test-home \
XDG_DATA_HOME=/tmp/firstcontact-test-data \
XDG_CONFIG_HOME=/tmp/firstcontact-test-config \
QT_QPA_PLATFORM=offscreen \
ASAN_OPTIONS=detect_leaks=0 \
make test BUILD_DIR=build/make-asan SANITIZERS=ON
```

`ASAN_OPTIONS=detect_leaks=0` is for environments where LeakSanitizer cannot
run cleanly, such as WSL or sandboxed/ptrace-like runners. AddressSanitizer and
UndefinedBehaviorSanitizer remain enabled.
