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

## Build dependencies

### Linux (Ubuntu 24.04+)

```bash
sudo apt install \
  qt6-base-dev qt6-base-dev-tools qt6-tools-dev qt6-tools-dev-tools \
  qt6-networkauth-dev libqt6sql6-sqlite \
  qt6-l10n-tools qt6-declarative-dev \
  libsecret-1-dev qtkeychain-qt6-dev \
  sqlite3 libsqlite3-dev \
  cmake ninja-build pkg-config build-essential
```

For the optional HTML rendering (Phase 3):

```bash
sudo apt install qt6-webengine-dev
```

### Windows

Install **Qt 6.7+** via the [Qt online installer](https://www.qt.io/download)
(MSVC 2022 64-bit) and **WiX Toolset** for MSI packaging. `qtkeychain-qt6`
isn't shipped by the Qt installer — pull it (and the optional `gtest`) via
the bundled `vcpkg.json` by setting `VCPKG_ROOT` before configuring with
`cmake --preset windows-msvc-release`.

### macOS

Install Xcode CLT and **Qt 6.7+** via the Qt online installer.

## Build

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
./build/linux-debug/src/app/firstcontact
```

Or without presets:

```bash
cmake -B build -G Ninja
cmake --build build
./build/src/app/firstcontact
```

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
