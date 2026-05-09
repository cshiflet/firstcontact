# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Branch topology

Two long-lived branches, both active:

- **`claude/native-gmail-client-2bqtq`** — single-account v1 (this worktree, `/home/user/firstcontact`).
- **`claude/multi-account`** — strict superset (worktree at `/home/user/firstcontact-multi`). The single-account branch is merged in at `c8b29e8`; multi-account v1/v2/v3/v4 commits (`1c31eb2`, `6ae1c61`, `f6ef20b`, `ddeb41f`) sit on top. New cross-cutting work should land on multi-account first; cherry-pick down only when it makes sense for the single-account branch.

The README still describes Phase 1 — out of date. Phases 1-3 are done on both branches. Phase 4 (packaging, CI matrix, signing, dark theme polish) is not started — no `.github/` yet.

## Build, test, run

`Makefile` wraps CMake. Common one-liners:

```
make setup     # apt/dnf/pacman install of Qt6 + deps + linuxdeploy
make build     # Debug build into ./build
make test      # ctest under offscreen Qt
make run       # launch from build tree
make run-dry   # launch with FC_DRY_RUN=1 — blocks destructive ops (delete, label edit,
               # star/archive/trash, pending-ops flush). Use for debugging without
               # mutating the live mailbox.
make smoke     # 4-second offscreen boot, exits 0 if event loop ran
make appimage  # Release build → AppImage (FirstContact-<ver>-<arch>.AppImage)
make format    # clang-format src/ tests/ in place
```

Knobs: `BUILD_TYPE=Release`, `SANITIZERS=ON` (ASan/UBSan in Debug). `make build SANITIZERS=ON` is the autonomous-review default.

CMake presets (`CMakePresets.json`): `linux-debug` (sanitizers on), `linux-release`, `windows-msvc-release`, `macos-release`. They use vcpkg toolchain via `$VCPKG_ROOT`.

**Single test:** `ctest --test-dir build -R test_html_sanitizer --output-on-failure` (or `make test ARGS="-R name"` won't work — call ctest directly). All tests need `QT_QPA_PLATFORM=offscreen` — the Makefile sets it; manual ctest invocations should too.

The build tree is at `./build` (Debug) and `./build-release` (Release). Smoke target deletes `~/.local/share/FirstContact/firstcontact.lock` first because the single-instance lock survives crashes.

## Architecture worth knowing before reading code

**Direct-to-Gmail, no backend.** OAuth 2.0 + PKCE (no client secret); the user supplies their own Google Cloud Desktop client_id via `SetupWizard`. Tokens live in QtKeychain via `TokenStore`. All Gmail traffic is `QNetworkAccessManager` REST through `src/api/RestClient.cpp`.

**Threading rule (Qt-mandated).** UI thread owns widgets/models. Sync thread owns `SyncService`, `RestClient`, repositories. **Each thread opens its own `QSqlDatabase`** — never share a connection across threads. UI ↔ Sync goes through queued signals carrying small POD payloads. Heavy parses (MIME, HTML sanitize) run on `QtConcurrent`.

**SQLite cache lives in WAL.** PRAGMAs (`journal_mode=WAL`, `foreign_keys=ON`) are applied unconditionally in `Migrations::run` *before* the per-step transactions — they cannot live inside a transaction, so they are not in `0001_init.sql`. Migrations are **forward-only**: each step is wrapped in `db.transaction()`/`commit()`, and `setSchemaVersion` `qFatal`s on failure (a half-applied schema is treated as fatal). Schema files at `resources/schema/0001_init.sql` … `0006_multi_account.sql`. Migration `0006` rebuilt every cache table with composite `(account_id, …)` keys plus per-account FTS5 — when adding a new table, follow that pattern.

**Sync is a small FSM** in `src/sync/SyncService.cpp`: `Idle ↔ {InitialSync, IncrementalSync, OutboxFlush, DraftSync, TopUpLabel}`. `topUpLabel` is the user-pull "fetch more older messages for this label" path — it sets `busy=true` synchronously, advances `pageToken` only after fetch success, and threads a `QPointer<SyncService>` through the inner `onOne` and `next` lambdas so a destroyed service doesn't run callbacks. Recursive `next` uses `weak_ptr<std::function>` outer + `shared_ptr` inner to break the lambda-capture cycle. Don't refactor those guards out.

**RestClient retry policy is method-aware:**
- `GET`, `PUT`: retry on transport failure, 5xx, 429.
- `POST`, `PATCH`, `DELETE`: retry **only on 429**.

This eliminates double-send on transient send failures. Don't widen retries on non-idempotent verbs.

**HTML rendering: no Chromium in our process.** WebEngine was removed at commit `4024534`. Current path: `HtmlSanitizer` (whitelist tags, strip scripts/iframes/forms/event-handlers, block remote images) → `LocalHtmlServer` (token-gated loopback HTTP, exact-path match) → `util::launchBrowser` hands the URL to the user's system browser. The server has a 5-min watchdog; re-clicks on the same message reuse the existing token. **Do not** add `QtWebEngineWidgets` back — the 39 MB private RAM idle baseline depends on its absence.

**Linkify pipeline** (`src/util/Linkify.cpp`) runs four passes in order: HTML anchor pre-pass (extract `<a>` and `<img alt>` from text/plain), markdown `[label](url)`, labeled `label [url]`, parenthesized `label (url)` (line-start-guarded so prose `(see https://…)` isn't a link), bare URL with start-end truncation. Display mode (Labeled / FullUrl) toggled with `Shift+L`.

**Multi-account model.** `AccountManager` owns N `AccountContext` (one per signed-in account). Each context has its own `OAuthClient` / `RestClient` / `GmailClient` / `SyncService` / scheduler. Repositories take `accountId` as the leading param; cross-account variants are `*AllAccounts`. `Database::defaultAccountId()` is the legacy fallback for code paths not yet account-aware. `MessageListModel::Source` enum picks `ByLabel` / `BySearch` / `CrossAccountLabel`.

**MainWindow helpers worth knowing:**
- `guardedThreadAction(dryRunKey, blockedStatus, successStatus, add, remove, refreshSidebar=false)` consolidates Archive/MarkRead/MarkUnread/Mute/Spam slots — use it for any new "apply label diff to current thread" action rather than open-coding the dry-run + UI plumbing.
- `reloadCurrentLabel` branches on source type (cross-account search → `replaceAll`, cross-account label → `setCrossAccountLabelSource`, single-account → `setLabelSource`, search → `setSearchSource`).

## Security-sensitive files (touch with care)

- `src/auth/OAuthClient.cpp` — PKCE flow, log scrubbing (authorize URL query, state values, token-exchange errors all redacted).
- `src/util/HtmlSanitizer.cpp` — quote-aware tag-end scan, 64 KiB scan cap, void-only self-close, nested-drop-tag depth tracking.
- `src/util/MimeBuilder.cpp` — RFC 2047 encoded-word splitting (UTF-8-safe ≤45-byte chunks), Message-ID dot-atom domain validation, Bcc smuggling defence.
- `src/api/MessageParser.cpp` — quote-aware address splitting (handles `"Last, First" <addr>`).
- `src/ui/reader/LocalHtmlServer.cpp` — exact-path token check; no prefix-match.
- `src/cache/MessageRepository.cpp` `applyLabelDiff` — transaction-wrapped, aborts on `db.transaction()` failure.

`docs/overnight-review-notes.md` lists items deliberately deferred from the autonomous-review pass — re-triage that list before opening a "fix everything" PR.

## Dependencies

System Qt 6.7+ (apt/dnf/pacman, never bundled via vcpkg). vcpkg supplies `qtkeychain` and `gtest` only — see `vcpkg.json`. AGPL-3.0-only.
