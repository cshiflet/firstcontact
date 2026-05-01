#include "Application.h"
#include "Bootstrap.h"

#include "ui/MainWindow.h"
#include "ui/common/Theme.h"
#include "util/Logger.h"
#include "util/Paths.h"

#include <QLockFile>

int main(int argc, char** argv) {
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
