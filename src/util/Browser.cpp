#include "Browser.h"

#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace fc::util {

namespace {

// True if we're running inside WSL (Windows Subsystem for Linux). Cached.
bool isWsl() {
    static bool checked = false;
    static bool result = false;
    if (!checked) {
        checked = true;
        if (!QProcessEnvironment::systemEnvironment()
                 .value(QStringLiteral("WSL_DISTRO_NAME")).isEmpty()) {
            result = true;
        } else {
            QFile f(QStringLiteral("/proc/version"));
            if (f.open(QIODevice::ReadOnly)) {
                const QByteArray v = f.readAll();
                result = v.contains("Microsoft") || v.contains("microsoft")
                      || v.contains("WSL");
            }
        }
        if (result) qInfo("util::launchBrowser: WSL environment detected");
    }
    return result;
}

// True if WSL has Windows interop reachable AND fully populated. False when
// /mnt/c isn't mounted at all OR when key Windows binaries are missing —
// the user might have a partially-mounted Windows drive where cmd.exe
// exists but reg.exe / powershell.exe don't, in which case wslview's
// internal default-browser lookup fails silently after QProcess::startDetached
// has already returned success. We'd then claim the launch worked and never
// fall through to the Linux launcher chain.
//
// Be strict here: if we can't be sure wslview will work end-to-end, skip
// the entire WSL block and let the Linux fallbacks (xdg-open, $BROWSER,
// firefox / chrome / chromium) handle it.
bool wslHasWindowsInterop() {
    auto present = [](const QString& path) {
        if (QFileInfo::exists(path)) return true;
        return !QStandardPaths::findExecutable(QFileInfo(path).fileName())
                    .isEmpty();
    };
    return present(QStringLiteral("/mnt/c/Windows/System32/cmd.exe"))
        && present(QStringLiteral("/mnt/c/Windows/System32/reg.exe"))
        && present(QStringLiteral(
               "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"));
}

}  // namespace

bool launchBrowser(const QUrl& url) {
    const QString u = url.toString();

    auto tryRun = [&](const QString& label, const QString& exe,
                      const QStringList& args) {
        if (QStandardPaths::findExecutable(exe).isEmpty()) return false;
        if (!QProcess::startDetached(exe, args)) return false;
        qInfo("util::launchBrowser: launched via '%s'", qUtf8Printable(label));
        return true;
    };

    // On WSL the Linux side typically has no browser — bridge to the Windows
    // host before falling back to Linux launchers. WSL2 forwards localhost
    // between Windows and the Linux side, so loopback URLs from our local
    // HTTP server (or the OAuth callback) work even though the URL is opened
    // in a process running on Windows.
    if (isWsl()) {
        if (!wslHasWindowsInterop()) {
            qInfo("util::launchBrowser: WSL Windows interop unavailable "
                  "(/mnt/c not mounted, or reg.exe / powershell.exe missing) "
                  "- falling through to Linux browser launchers");
        } else {
            if (tryRun("wslview", "wslview", {u})) return true;
            if (tryRun("cmd.exe",        "cmd.exe",
                       {"/c", "start", "", u})) return true;
            if (tryRun("powershell.exe", "powershell.exe",
                       {"-NoProfile", "-Command",
                        QStringLiteral("Start-Process '%1'").arg(u)})) return true;
        }
    }

    if (QDesktopServices::openUrl(url)) {
        qInfo("util::launchBrowser: launched via QDesktopServices");
        return true;
    }

    const QString browserEnv =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("BROWSER"));
    if (!browserEnv.isEmpty()
        && tryRun("$BROWSER", browserEnv, {u})) return true;

    if (tryRun("xdg-open",         "xdg-open",         {u})) return true;
    if (tryRun("sensible-browser", "sensible-browser", {u})) return true;
    if (tryRun("gio open",         "gio",              {"open", u})) return true;
    if (tryRun("kde-open5",        "kde-open5",        {u})) return true;
    if (tryRun("gnome-open",       "gnome-open",       {u})) return true;

    for (const QString& exe : {QStringLiteral("firefox"),
                                QStringLiteral("google-chrome"),
                                QStringLiteral("chromium"),
                                QStringLiteral("chromium-browser"),
                                QStringLiteral("brave-browser"),
                                QStringLiteral("microsoft-edge")}) {
        if (tryRun(exe, exe, {u})) return true;
    }

    qWarning("util::launchBrowser: every strategy failed for %s",
             qUtf8Printable(u));
    return false;
}

}  // namespace fc::util
