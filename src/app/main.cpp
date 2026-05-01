#include "Application.h"
#include "Bootstrap.h"

#include "ui/MainWindow.h"
#include "ui/common/Preferences.h"
#include "ui/common/Theme.h"
#include "util/Logger.h"
#include "util/Paths.h"

#include <QCoreApplication>
#include <QLibrary>
#include <QLockFile>
#include <QSettings>

int main(int argc, char** argv) {
    // Qt WebEngine requires Qt::AA_ShareOpenGLContexts to be set BEFORE
    // QApplication is constructed; setting it unconditionally is a no-op
    // when WebEngine is never used.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // We need to read the html-preview preference before QApplication is
    // constructed (so we can decide whether to pre-load Qt6WebEngineCore).
    // QSettings keys off the org/app names, so set those first — the same
    // values Application::Application sets later.
    QCoreApplication::setOrganizationName(QStringLiteral("FirstContact"));
    QCoreApplication::setApplicationName(QStringLiteral("FirstContact"));

    // Only pay the WebEngine pre-load cost (≈50–80 MB resident in this
    // process plus Chromium init time) when the user has explicitly opted
    // into the inline rich preview. Disabled and external-browser modes
    // never need WebEngine, so they get a leaner idle.
    {
        QSettings s;
        const auto mode = fc::ui::Preferences::htmlPreviewFromString(
            s.value(QLatin1String(fc::ui::Preferences::htmlPreviewKey()),
                    QStringLiteral("external")).toString());
        if (mode == fc::ui::Preferences::HtmlPreview::InlineWebEngine) {
            QLibrary core(QStringLiteral("Qt6WebEngineCore"), 6);
            if (core.load()) {
                qInfo("Qt6WebEngineCore pre-loaded for inline HTML preview");
            } else {
                qInfo("Inline HTML preview requested but Qt6WebEngineCore is "
                      "unavailable: %s — falling back to external-browser mode "
                      "at runtime", qUtf8Printable(core.errorString()));
            }
        } else {
            qInfo("HTML preview mode: %s — WebEngine pre-load skipped",
                  qUtf8Printable(fc::ui::Preferences::htmlPreviewToString(mode)));
        }
    }

    fc::app::Application app(argc, argv);
    fc::util::installLogger();

    // Apply the theme BEFORE constructing widgets so the QSS lands on the
    // initial paint and we don't briefly flash the system's default style.
    fc::ui::Theme::applyPersisted();

    QLockFile lock(fc::util::singleInstanceLockPath());
    lock.setStaleLockTime(30'000);   // 30s — clear locks left by crashed processes
    if (!lock.tryLock(0)) {
        qWarning("Another FirstContact instance is already running.");
        return 0;
    }

    fc::app::Bootstrap boot;
    boot.mainWindow()->show();
    return app.exec();
}
