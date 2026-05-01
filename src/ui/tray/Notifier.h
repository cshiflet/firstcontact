#pragma once

#include <QObject>
#include <QString>

class QSystemTrayIcon;

namespace fc::ui {

// Cross-platform notification facade. Falls back to QSystemTrayIcon::showMessage
// when the platform has no native toast API; on Linux this uses libnotify via
// QSystemTrayIcon, on Windows it routes to WinRT toasts when available, on
// macOS to NSUserNotificationCenter via Qt.
class Notifier : public QObject {
    Q_OBJECT
public:
    Notifier(QSystemTrayIcon* tray, QObject* parent = nullptr);

    void notifyNewMail(int count, const QString& latestSender,
                       const QString& latestSubject,
                       const QString& threadIdToOpen);

signals:
    // Emitted when the user activates a notification (e.g. clicks the toast).
    // Phase 3 wires this to MainWindow to focus the relevant thread.
    void openThreadRequested(const QString& threadId);

private:
    QSystemTrayIcon* tray_;
    QString          pendingThreadId_;
};

}  // namespace fc::ui
