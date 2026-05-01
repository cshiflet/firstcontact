#include "Application.h"
#include "Bootstrap.h"

#include "ui/MainWindow.h"
#include "ui/common/Theme.h"
#include "util/Logger.h"
#include "util/Paths.h"

#include <QCoreApplication>
#include <QLibrary>
#include <QLockFile>

int main(int argc, char** argv) {
    // Qt WebEngine requires Qt::AA_ShareOpenGLContexts to be set BEFORE
    // QApplication is constructed; setting it unconditionally is a no-op
    // when WebEngine is never used.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // If Qt6WebEngineCore is installed, pre-load it now so its static
    // initializers run before QApplication. Without this, dynamically
    // loading the firstcontact_html_renderer plugin via QLibrary later in
    // the run trips the "WebEngine seems to be initialized from a plugin"
    // diagnostic and segfaults during the first QWebEngineView construction.
    // No-op (with an info log) on systems without WebEngine.
    {
        QLibrary core(QStringLiteral("Qt6WebEngineCore"), 6);
        if (!core.load()) {
            qInfo("Qt6WebEngineCore not available — 'Show full HTML' will be "
                  "unavailable: %s", qUtf8Printable(core.errorString()));
        } else {
            qInfo("Qt6WebEngineCore pre-loaded for HTML rendering");
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
