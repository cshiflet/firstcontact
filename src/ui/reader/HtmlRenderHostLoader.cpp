#include "HtmlRenderHostLoader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QStringList>

namespace fc::ui {

namespace {

QLibrary& gLib() {
    static QLibrary lib;
    return lib;
}

QMutex& gMutex() {
    static QMutex m;
    return m;
}

bool gInitTried = false;
fc_html_renderer_create_fn gFactory = nullptr;

QStringList candidatePaths() {
    QStringList out;
    const QString appDir = QCoreApplication::applicationDirPath();
    // Production deployment locations.
    out << appDir + QStringLiteral("/libfirstcontact_html_renderer.so")
        << appDir + QStringLiteral("/../lib/libfirstcontact_html_renderer.so")
        << appDir + QStringLiteral("/firstcontact_html_renderer.dll")
        << appDir + QStringLiteral("/../Frameworks/libfirstcontact_html_renderer.dylib");
#ifndef NDEBUG
    // Convenience for running directly from the build tree during development.
    // Excluded from release so a malicious .so dropped at this relative
    // location can't be silently loaded by an installed binary.
    out << appDir + QStringLiteral("/../ui/reader/webengine_plugin/"
                                   "libfirstcontact_html_renderer.so");
#endif
    return out;
}

void tryLoadLocked() {
    if (gInitTried) return;
    gInitTried = true;
    for (const QString& path : candidatePaths()) {
        if (!QFileInfo::exists(path)) continue;
        gLib().setFileName(path);
        if (!gLib().load()) {
            qWarning("HtmlRenderHost: load failed for %s: %s",
                     qUtf8Printable(path),
                     qUtf8Printable(gLib().errorString()));
            continue;
        }
        gFactory = reinterpret_cast<fc_html_renderer_create_fn>(
            gLib().resolve("fc_create_html_renderer"));
        if (gFactory) return;
        qWarning("HtmlRenderHost: missing fc_create_html_renderer in %s",
                 qUtf8Printable(path));
        gLib().unload();
    }
}

}  // namespace

bool HtmlRenderHostLoader::available() {
    QMutexLocker lock(&gMutex());
    tryLoadLocked();
    return gFactory != nullptr;
}

IHtmlRenderHost* HtmlRenderHostLoader::create(QWidget* parent) {
    fc_html_renderer_create_fn factory = nullptr;
    {
        QMutexLocker lock(&gMutex());
        tryLoadLocked();
        factory = gFactory;
    }
    if (!factory) return nullptr;
    return factory(parent);
}

}  // namespace fc::ui
