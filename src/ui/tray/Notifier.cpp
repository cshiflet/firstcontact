#include "Notifier.h"

#include <QSystemTrayIcon>

namespace fc::ui {

Notifier::Notifier(QSystemTrayIcon* tray, QObject* parent)
    : QObject(parent), tray_(tray) {
    if (tray_) {
        connect(tray_, &QSystemTrayIcon::messageClicked, this, [this] {
            if (!pendingThreadId_.isEmpty()) {
                emit openThreadRequested(pendingThreadId_);
                pendingThreadId_.clear();
            }
        });
    }
}

void Notifier::notifyNewMail(int count, const QString& latestSender,
                             const QString& latestSubject,
                             const QString& threadIdToOpen) {
    if (!tray_ || !QSystemTrayIcon::supportsMessages()) return;

    pendingThreadId_ = threadIdToOpen;

    const QString title = count == 1
        ? tr("New message from %1").arg(latestSender)
        : tr("%1 new messages").arg(count);
    const QString body = count == 1 ? latestSubject : latestSender;

    tray_->showMessage(title, body, QSystemTrayIcon::Information, 4000);
}

void Notifier::notifyError(const QString& title, const QString& body) {
    if (!tray_ || !QSystemTrayIcon::supportsMessages()) return;
    // Errors don't have a thread to focus, so clear pending state so a
    // following messageClicked doesn't accidentally jump into the
    // last new-mail thread.
    pendingThreadId_.clear();
    // 8 s — long enough to read, short enough to fit inside most
    // platforms' toast budgets. Body trimmed to avoid the toast
    // running off the edge of macOS / Win11 popovers.
    QString shortBody = body;
    if (shortBody.size() > 240) shortBody = shortBody.left(237) + QLatin1String("…");
    tray_->showMessage(title, shortBody, QSystemTrayIcon::Critical, 8000);
}

}  // namespace fc::ui
