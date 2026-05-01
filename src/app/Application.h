#pragma once

#include <QApplication>

namespace fc::app {

// QApplication subclass that sets organization/app metadata so QSettings,
// QStandardPaths, and the Wayland app-id all resolve consistently.
class Application : public QApplication {
    Q_OBJECT
public:
    Application(int& argc, char** argv);
};

}  // namespace fc::app
