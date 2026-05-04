#pragma once

#include <functional>

class QObject;
class QUrl;

namespace fc::util {

// Fire-and-forget URL launcher. Walks an increasingly-broad chain of
// strategies and stops at the first one that reports success:
//
//   On WSL with /mnt/c reachable: wslview → cmd.exe → powershell.exe
//   Then:  QDesktopServices::openUrl (Qt's preferred path)
//   Then:  $BROWSER → xdg-open → sensible-browser → gio open →
//          kde-open5 → gnome-open
//   Then:  firefox → google-chrome → chromium → brave-browser → microsoft-edge
//
// The chain runs on QThreadPool::globalInstance() — it does NOT block the
// calling thread. That matters because at least one rung in the chain
// (xdg-open / sensible-browser / gio open) is launched synchronously to
// capture its real exit code, and on WSL setups where wslview hangs the
// blocking sub-launcher used to freeze the UI for tens of seconds.
//
// Used by:
//   - OAuthClient (sign-in: open Google's consent page)
//   - ReaderPane (rich-text anchor clicks; "Open in browser" preview)
//
// Logs the strategy that worked (or that all failed) on the worker thread.
void launchBrowser(const QUrl& url);

// Same as launchBrowser, but invokes `onComplete(ok)` on `context`'s thread
// when the chain finishes — `ok=true` if any strategy reported success.
// Used by callers that want to update the UI based on the result (e.g.
// retry button enable/disable). If `context` is destroyed before the chain
// finishes the callback is silently dropped.
void launchBrowser(const QUrl& url, QObject* context,
                    std::function<void(bool)> onComplete);

}  // namespace fc::util
