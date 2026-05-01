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

}  // namespace fc::ui
