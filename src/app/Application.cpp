#include "Application.h"

namespace fc::app {

Application::Application(int& argc, char** argv) : QApplication(argc, argv) {
    setOrganizationName(QStringLiteral("FirstContact"));
    setOrganizationDomain(QStringLiteral("firstcontact.org"));
    setApplicationName(QStringLiteral("FirstContact"));
    setApplicationDisplayName(QStringLiteral("FirstContact"));
    setApplicationVersion(QStringLiteral("0.1.0"));
    setDesktopFileName(QStringLiteral("firstcontact"));
}

}  // namespace fc::app
