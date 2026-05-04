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

    // tryRun: fork-and-forget. Used for direct browser names where we
    // don't care about exit code; the browser process keeps running long
    // after our forked launcher returns.
    auto tryRun = [&](const QString& label, const QString& exe,
                      const QStringList& args) {
        const QString resolved = QStandardPaths::findExecutable(exe);
        if (resolved.isEmpty()) {
            qInfo("util::launchBrowser: skip '%s' — '%s' not on PATH",
                  qUtf8Printable(label), qUtf8Printable(exe));
            return false;
        }
        qint64 pid = 0;
        if (!QProcess::startDetached(exe, args, QString(), &pid)) {
            qInfo("util::launchBrowser: '%s' (%s) failed to start",
                  qUtf8Printable(label), qUtf8Printable(resolved));
            return false;
        }
        qInfo("util::launchBrowser: launched via '%s' (%s, pid=%lld)",
              qUtf8Printable(label), qUtf8Printable(resolved), pid);
        return true;
    };

    // tryRunSync: launches synchronously and inspects the exit code. Used
    // for indirection-layer launchers (xdg-open, sensible-browser, gio
    // open) where the process exits as soon as it has dispatched the URL
    // — a non-zero code is the only honest signal that it failed. With
    // tryRun (startDetached) we'd see "succeeded" even when xdg-open
    // returns exit 4 ("no application found"), since the fork itself
    // completed. Capped at 10 s; xdg-open typically exits in well under
    // 100 ms either way.
    auto tryRunSync = [&](const QString& label, const QString& exe,
                          const QStringList& args) {
        const QString resolved = QStandardPaths::findExecutable(exe);
        if (resolved.isEmpty()) {
            qInfo("util::launchBrowser: skip '%s' — '%s' not on PATH",
                  qUtf8Printable(label), qUtf8Printable(exe));
            return false;
        }
        const int rc = QProcess::execute(exe, args);
        if (rc == 0) {
            qInfo("util::launchBrowser: launched via '%s' (%s, exit 0)",
                  qUtf8Printable(label), qUtf8Printable(resolved));
            return true;
        }
        qInfo("util::launchBrowser: '%s' (%s) returned exit %d — trying next",
              qUtf8Printable(label), qUtf8Printable(resolved), rc);
        return false;
    };

    static const QStringList kKnownBrowsers = {
        QStringLiteral("firefox"),
        QStringLiteral("google-chrome"),
        QStringLiteral("chromium"),
        QStringLiteral("chromium-browser"),
        QStringLiteral("brave-browser"),
        QStringLiteral("microsoft-edge"),
    };
    const QString browserEnv =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("BROWSER"));

    // Tier 1: WSL → Windows browser via wslview / cmd.exe / powershell.exe.
    // Only attempted when /mnt/c has the binaries wslview needs internally;
    // otherwise wslview "starts" (forks) and fails downstream, which
    // QProcess::startDetached can't tell us about.
    if (isWsl()) {
        if (!wslHasWindowsInterop()) {
            qInfo("util::launchBrowser: WSL Windows interop unavailable "
                  "(/mnt/c not mounted, or reg.exe / powershell.exe missing) "
                  "- skipping wslview/cmd.exe/powershell.exe; xdg-open and "
                  "friends will be tried synchronously below, then $BROWSER "
                  "and direct browser names");
        } else {
            if (tryRun("wslview", "wslview", {u})) return true;
            if (tryRun("cmd.exe",        "cmd.exe",
                       {"/c", "start", "", u})) return true;
            if (tryRun("powershell.exe", "powershell.exe",
                       {"-NoProfile", "-Command",
                        QStringLiteral("Start-Process '%1'").arg(u)})) return true;
        }
        // On WSL the xdg-open / gio / etc. layer typically delegates to
        // wslview — but if the user has installed a Linux browser AND
        // configured xdg-mime to point at it (or wslview was uninstalled),
        // xdg-open will hand the URL to that browser directly. Try it,
        // but synchronously so we can see the exit code: xdg-open returns
        // 4 when no application is found, which startDetached would
        // happily report as "launched". Same logic for sensible-browser
        // and gio open — all are layered launchers that exit quickly with
        // a real status.
        if (tryRunSync("xdg-open",         "xdg-open",         {u})) return true;
        if (tryRunSync("sensible-browser", "sensible-browser", {u})) return true;
        if (tryRunSync("gio open",         "gio",              {"open", u})) return true;

        if (!browserEnv.isEmpty()
            && tryRun("$BROWSER", browserEnv, {u})) return true;
        for (const QString& exe : kKnownBrowsers) {
            if (tryRun(exe, exe, {u})) return true;
        }
        qWarning("util::launchBrowser: every WSL-safe strategy failed for %s",
                 qUtf8Printable(u));
        return false;
    }

    // Tier 2 (non-WSL): system default via QDesktopServices.
    if (QDesktopServices::openUrl(url)) {
        qInfo("util::launchBrowser: launched via QDesktopServices");
        return true;
    }

    // Tier 3 (non-WSL): explicit user preference via $BROWSER.
    if (!browserEnv.isEmpty()
        && tryRun("$BROWSER", browserEnv, {u})) return true;

    // Tier 4 (non-WSL): the xdg-open / desktop-environment opener chain.
    if (tryRun("xdg-open",         "xdg-open",         {u})) return true;
    if (tryRun("sensible-browser", "sensible-browser", {u})) return true;
    if (tryRun("gio open",         "gio",              {"open", u})) return true;
    if (tryRun("kde-open5",        "kde-open5",        {u})) return true;
    if (tryRun("gnome-open",       "gnome-open",       {u})) return true;

    // Tier 5: known browsers by literal name. Last resort — works on a
    // minimal install with no desktop integration at all, as long as one
    // of these is on PATH.
    for (const QString& exe : kKnownBrowsers) {
        if (tryRun(exe, exe, {u})) return true;
    }

    qWarning("util::launchBrowser: every strategy failed for %s",
             qUtf8Printable(u));
    return false;
}

}  // namespace fc::util
