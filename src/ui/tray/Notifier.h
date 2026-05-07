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
    enum class NewMailMode {
        ArrivalOnly,    // "New mail in chris@example.com" — privacy default
        Preview,        // "New message from <sender>" / subject body
    };

    Notifier(QSystemTrayIcon* tray, QObject* parent = nullptr);

    // accountEmail is always rendered so the user can tell which
    // account a multi-account toast belongs to. Pre-multi-account
    // call sites can pass an empty accountEmail; the toast then
    // omits the account suffix.
    void notifyNewMail(NewMailMode mode,
                       const QString& accountEmail,
                       int count,
                       const QString& latestSender,
                       const QString& latestSubject,
                       const QString& threadIdToOpen);

    // Surface a Critical-priority toast (system tray on platforms that
    // support it) for unrecoverable errors the user should know about
    // — sync failure, send failure, etc. Body should be short; we cap
    // it for platforms with strict toast budgets.
    void notifyError(const QString& title, const QString& body);

signals:
    // Emitted when the user activates a notification (e.g. clicks the toast).
    // Phase 3 wires this to MainWindow to focus the relevant thread.
    void openThreadRequested(const QString& threadId);

private:
    QSystemTrayIcon* tray_;
    QString          pendingThreadId_;
};

}  // namespace fc::ui
