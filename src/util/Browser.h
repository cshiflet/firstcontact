#pragma once

class QUrl;

namespace fc::util {

// Best-effort opener for a URL in the user's preferred browser.
//
// Used by the OAuth sign-in flow to launch Google's consent page and by
// ReaderPane to open a locally-served HTML message preview. Walks an
// increasingly-broad chain of strategies and returns true the first time
// QProcess::startDetached succeeds:
//
//   On WSL with /mnt/c reachable: wslview → cmd.exe → powershell.exe
//   Then: QDesktopServices::openUrl (Qt's preferred path)
//   Then: $BROWSER → xdg-open → sensible-browser → gio open →
//         kde-open5 → gnome-open
//   Then: firefox → google-chrome → chromium → brave-browser → microsoft-edge
//
// Logs the strategy that worked (or that all failed) at info / warning level.
bool launchBrowser(const QUrl& url);

}  // namespace fc::util
