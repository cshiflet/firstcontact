#include "Application.h"
#include "Bootstrap.h"

#include "ui/MainWindow.h"
#include "ui/common/Theme.h"
#include "util/Logger.h"
#include "util/Paths.h"

#include <QCoreApplication>
#include <QLockFile>

int main(int argc, char** argv) {
    // QSettings keys off the org/app names; set them BEFORE QApplication
    // so any pre-construction reads land in the right .conf file.
    // (Used to also pre-load Qt6WebEngineCore here when the inline HTML
    // preview was selected — that path was removed; see the "Drop
    // inline WebEngine HTML preview" commit.)
    QCoreApplication::setOrganizationName(QStringLiteral("FirstContact"));
    QCoreApplication::setApplicationName(QStringLiteral("FirstContact"));

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
