# Autonomous Review — Overnight Pass

Twelve commits between the user's "review autonomously overnight" message and morning. Four review iterations, each spawning fresh subagents to hunt for a different class of issue, with fixes shipped between rounds. All commits preserve passing tests (12/12 throughout).

Branch: `claude/native-gmail-client-2bqtq`. Pushed after each commit.

## What landed

### Round 1 — Initial sweep
- `cd8e772` Linkify HTML-anchor pre-pass for raw `<a>` in text/plain
- `deab611` HtmlSanitizer quote-aware tag-end scan + void-only self-close
- `273e1ca` MessageParser quote-aware address splitting
- `0704719` Linkify `label (URL)` handler + `<img alt>` extraction + first SyncService race fix
- `8bb5e26` DB integrity: per-step migration transactions + `applyLabelDiff` transaction wrap
- `d2d6bcb` Model + UI + MIME polish — model re-entry guard, expandedThreads_ preserved, MimeBuilder encoded-word splitting + Message-ID domain validation
- `cad4693` Worker QPointer guards (OutboxWorker, DraftSync)
- `a05f663` MainWindow `guardedThreadAction` helper extraction (~50 lines of duplication removed)

### Round 2 — Follow-up findings against the post-fix code
- `d70e35e` Four HIGH-severity follow-ups: PendingOpsWorker `onDone` QPointer; SyncService `fetchAndStoreMessages` raw-`this` capture; MessageRepository abandoning writes when `db.transaction()` fails; `refreshFromSource` counting only parent rows when sizing the probe limit. Plus Linkify polish (`parenLinkRe` line-start guard, `final` rename, Base64Url error logging).

### Round 3 — Tests + auth/REST sweep
- `418ec9f` Tests: nested drop-tag depth in HtmlSanitizer; Bcc smuggling defence in MimeBuilder
- `86535d6` HIGH findings: idempotent-only retries in RestClient (POST / PATCH / DELETE no longer auto-retry on transport failure or 5xx — eliminates double-send risk); OAuth log scrubbing (authorize URL query, state values, token-exchange body); TokenStore parse-failure surfaces as error rather than masquerading as "no credentials"

### Round 4 — Reader / compose polish
- `7f60b0f` LocalHtmlServer exact-path token check (no more prefix-match through `/<token>anything`); ComposeWindow find-from-document-start for the cursor marker; HtmlSanitizer 64 KiB scan cap so a malformed unclosed quote can't swallow the document tail.

## Summary by category

| Category | Items |
|---|---|
| Security | OAuth log scrubbing • LocalHtmlServer path-exact match • HtmlSanitizer scan cap • idempotent-only retries (no double-send) |
| Threading / lifecycle | QPointer guards on OutboxWorker, DraftSync, PendingOpsWorker, SyncService inner lambdas • SyncService busy-set-synchronously race fix • SyncService weak_ptr cycle break in fetchAndStoreMessages |
| Data integrity | per-step migration transactions • applyLabelDiff transaction wrap with abort on begin-failure • Message-ID domain validation |
| Correctness | quote-aware address splitting • quote-aware HTML tag-end scan • encoded-word RFC 2047 splitting • topUpLabel pageToken advance after fetch success • refreshFromSource probe counts parent rows • model re-entry guard • expanded-thread preservation across refresh |
| UX | linkify HTML anchor + img alt + parenthesized + line-start guard + label/url toggle (`Shift+L`) + hover URL in status bar • TokenStore parse-failure surfaces |
| Code quality | guardedThreadAction helper • Base64Url error logging • renamed shadowing `final` identifier |
| Tests | nested drop-tag, Bcc smuggling, encoded-word splitting, Message-ID malformed domain, quoted address splits, parenthesized link, img-alt anchor, HTML anchor pre-pass, markdown link |

## Items deliberately deferred

These came up across the four reviews and are documented here so they're not re-reviewed by accident. Re-triaged 2026-05-09 — every item below is still applicable; nothing was implicitly closed by the round-3/4 fixes.

- **OAuthClient redirect-URI hardcode to `127.0.0.1`** (RFC 8252). Qt's `QOAuthHttpServerReplyHandler` default is already `127.0.0.1`, so the practical risk is small; explicit-set would risk breaking existing user OAuth registrations that point at `localhost`. Marginal security benefit on the user's own loopback. Status: still deferred.
- **OAuthClient refresh-race serialization**. The existing race is wasteful (two concurrent refreshes possible) but produces no data corruption — both writers land valid tokens via the existing mutex. Fix is invasive (cross-thread waiting on a nested QEventLoop is fragile in Qt). Status: still deferred.
- **MimeBuilder `formatAddressList` non-ASCII display names**. Recipients are passed pre-formatted as plain strings; encoded-word handling is on the caller. Document-the-contract rather than auto-encode. Status: still deferred.
- **MessageRepository.cpp:269 silent error on listByLabel `q.exec`**. Low-impact; UI sees empty list either way. Status: still deferred.
- **Logger.cpp:32-39 rotation race on Windows**. Linux path is fine; Windows isn't a primary target yet. Status: still deferred.
- **Browser.cpp non-WSL fallback chain**. Synchronous `tryRunSync` already used on the WSL branch; non-WSL path uses Qt defaults and rarely fails. Status: still deferred.
- **Helper extraction for the worker `flush()` skeleton** (OutboxWorker / DraftSync / PendingOpsWorker share ~30 lines each). Saves duplication but doesn't fix any bug. Status: still deferred.

### Added 2026-05-09 (post-pass)

- **Qt 6.4 QNAM transparent retry on POST/PATCH/DELETE transport failure.** Surfaced while writing `test_rest_client`. When the TCP connection drops before any HTTP response bytes, QNAM transparently retries the request — for ALL verbs, including non-idempotent ones — by calling `reset()` on the upload byte device. This happens BELOW our application-layer retry policy: we never see the original failure. Concretely, a flaky network that drops the connection before Gmail responds to `messages.send` could silently double-send. The round-3 idempotent-only guard still protects against the more common 5xx case (server wrote bytes → QNAM does not auto-retry), so the residual hole is "TCP dropped before any response" — rare on Gmail's well-provisioned endpoints but not impossible. `test_rest_client::qnamTransparentlyRetriesPostOnTransportFailure_documented` is the canary; if a future Qt release changes the behaviour, that test starts failing and we revisit. Possible mitigations to investigate when this becomes worth fixing: send `Connection: close` on non-idempotent verbs to defeat keep-alive reuse, route those verbs through a custom `QIODevice` whose `reset()` returns false, or move to a different HTTP client for the send path.

## Build / test status

13 unit tests pass under `QT_QPA_PLATFORM=offscreen ctest` (test_rest_client added 2026-05-09). Build clean with `-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wold-style-cast -Wcast-align -Wunused`.
