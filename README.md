# FirstContact

A native, resource-efficient Gmail client built on **Qt 6** (C++).

FirstContact talks to Gmail directly via the Gmail REST API — no third-party
backend. It mirrors the philosophy of [baremail](https://github.com/cshiflet/baremail)
(direct-to-Gmail PWA, lazy resource use, plain-text-first) but runs as a true
native desktop application with a richer Gmail-web-style three-pane UI.

**Status:** Phase 1 (MVP scaffold). Sign in, list inbox, read plain-text bodies.
Cache, sync, compose, search, and HTML-on-demand land in Phases 2–3.

## Goals

- Native UI on Linux (priority), Windows, and macOS.
- Idle RAM under **80 MB** (under 200 MB when the optional HTML webview is engaged).
- SQLite + FTS5 for offline read and search.
- OAuth 2.0 + PKCE using your own Google Cloud Desktop-app Client ID and Client Secret.
- Plain-text reading by default; webview only when you click "Show full HTML".

## Toolchains and setup

FirstContact builds with CMake + Ninja and Qt 6. The repo includes
idempotent setup targets/scripts for the supported developer platforms.

### Linux

Ubuntu/Debian, Fedora, and Arch/Manjaro are supported through the Makefile:

```bash
make setup
```

`make setup` installs the distro Qt packages, qtkeychain, zstd, SQLite,
compiler tooling, test helpers, and Linux AppImage tooling. You can also call
the distro-specific target directly:

```bash
make setup-ubuntu
make setup-fedora
make setup-arch
```

### Windows 11 Pro

Use **Developer PowerShell for VS 2022**. The setup script installs missing
tools via `winget`, installs Qt with `aqtinstall`, bootstraps vcpkg, installs
the manifest dependencies (`qtkeychain-qt6`, `zstd`), and writes local CMake
presets to `CMakeUserPresets.json`.

```powershell
powershell -ExecutionPolicy Bypass -File scripts/setup-windows.ps1
cmake --preset windows-msvc-debug-local
cmake --build --preset windows-msvc-debug-local
ctest --preset windows-msvc-debug-local
```

Notes:

- The default Qt version is `6.7.3`. Override it before setup with
  `$env:FC_QT_VERSION = "6.x.y"` if needed.
- The default tool root is `%USERPROFILE%\firstcontact-tools`.
- MSI packaging uses CPack/WiX metadata, but signing is intentionally separate
  from local developer builds.

### macOS

Install Xcode Command Line Tools first, then run the setup target or script:

```bash
xcode-select --install
make setup
```

The macOS setup installs missing Homebrew packages (`cmake`, `ninja`, `qt`,
`qtkeychain`, `zstd`, etc.) and writes local CMake presets to
`CMakeUserPresets.json`.

```bash
cmake --preset macos-debug-local
cmake --build --preset macos-debug-local
ctest --preset macos-debug-local
```

## Build

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
./build/linux-debug/src/app/firstcontact
```

Or through the Makefile on Linux, or on any shell where CMake can already find
the installed Qt/dependency prefixes:

```bash
make build
make test
```

Or without presets:

```bash
cmake -B build -G Ninja
cmake --build build
./build/src/app/firstcontact
```

## Pre-release database policy

FirstContact is still pre-release. The app creates fresh caches from the
current consolidated SQLite schema and rejects older pre-release database
versions instead of migrating them. If startup reports an old database version,
run:

```bash
firstcontact reset-db
```

This discards the local cache and lets the next launch create a fresh database.
Schema history and supported forward migrations will be added with the first
supported release. See [docs/multi-account-plan.md](docs/multi-account-plan.md)
for the multi-account hardening notes.

## First-run setup

FirstContact uses your own Google OAuth Desktop-app Client ID and Client Secret:

1. Open the [Google Cloud Console](https://console.cloud.google.com/apis/credentials).
2. Create (or pick) a project and **enable the Gmail API**.
3. Configure the **OAuth consent screen** (External, add your email as a test user).
4. Create credentials → **OAuth client ID** → application type **Desktop app**.
5. Copy both the **Client ID** and **Client Secret** from the credential details.
   FirstContact uses PKCE, but Google's Desktop-app token endpoint still requires
   the client secret value.
6. Launch FirstContact. The setup wizard prompts for the Client ID and Client
   Secret. Click **Sign In**, consent in your browser, and you're done.

## Layout

```
src/
  app/      main, QApplication, service wiring
  auth/     OAuth + PKCE, QtKeychain token store, client_id/client_secret config
  api/      QNAM REST client, Gmail endpoints, batch, history, parser
  sync/     (Phase 2) initial + incremental sync, outbox, draft sync
  cache/    (Phase 2) SQLite + FTS5 schema, repositories, LRU evictor
  models/   POD types + Qt item models
  ui/       three-pane shell, sidebar, list view, reader, compose, setup wizard
  util/     paths, base64url, logger, linkify, html2text
resources/
  schema/   SQL migrations bundled into the binary via Qt resources
tests/
  unit/     QtTest cases (parser, sanitizer, base64url, rate limiter, etc.)
  integration/ (Phase 2) OAuth + sync end-to-end against fakes
packaging/  AppImage, Flatpak, MSI, DMG metadata
```

## License

AGPL-3.0-only. See `LICENSE`.
