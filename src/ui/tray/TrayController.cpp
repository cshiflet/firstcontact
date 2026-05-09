#include "TrayController.h"

#include "Notifier.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMainWindow>
#include <QMenu>
#include <QStyle>
#include <QSystemTrayIcon>

namespace fc::ui {

TrayController::TrayController(QMainWindow* main, QObject* parent)
    : QObject(parent), mainWindow_(main) {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        tray_ = nullptr;
        notifier_ = new Notifier(nullptr, this);
        return;
    }

    tray_ = new QSystemTrayIcon(this);
    tray_->setIcon(qApp->style()->standardIcon(QStyle::SP_MessageBoxInformation));
    tray_->setToolTip(QStringLiteral("FirstContact"));

    // Parentless QMenu is fine here — setContextMenu below transfers
    // ownership to tray_, which is a child of this QObject, so the
    // menu is reaped on TrayController destruction.
    auto* menu = new QMenu();
    auto* compose = menu->addAction(tr("Compose…"));
    auto* refresh = menu->addAction(tr("Refresh"));
    menu->addSeparator();
    auto* quit    = menu->addAction(tr("Quit"));

    connect(compose, &QAction::triggered, this, &TrayController::composeRequested);
    connect(refresh, &QAction::triggered, this, &TrayController::refreshRequested);
    connect(quit,    &QAction::triggered, this, &TrayController::quitRequested);

    tray_->setContextMenu(menu);

    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason r) { onActivated(int(r)); });

    notifier_ = new Notifier(tray_, this);
    tray_->show();
}

Notifier* TrayController::notifier() const { return notifier_; }
bool TrayController::available() const { return tray_ != nullptr; }

void TrayController::onActivated(int reason) {
    if (reason == QSystemTrayIcon::Trigger ||
        reason == QSystemTrayIcon::DoubleClick) {
        if (mainWindow_) {
            mainWindow_->show();
            mainWindow_->raise();
            mainWindow_->activateWindow();
        }
    }
}

}  // namespace fc::ui
