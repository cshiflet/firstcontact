#pragma once

#include <QObject>

class QMainWindow;
class QSystemTrayIcon;

namespace fc::ui {

class Notifier;

// Sets up the QSystemTrayIcon, its right-click menu, and handlers for click
// and double-click. Owns a Notifier the rest of the app can pull mail-arrival
// toasts through.
class TrayController : public QObject {
    Q_OBJECT
public:
    TrayController(QMainWindow* main, QObject* parent = nullptr);

    Notifier* notifier() const;
    bool      available() const;

signals:
    void composeRequested();
    void refreshRequested();
    void quitRequested();

private slots:
    void onActivated(int reason);   // QSystemTrayIcon::ActivationReason

private:
    QMainWindow*     mainWindow_;
    QSystemTrayIcon* tray_;
    Notifier*        notifier_;
};

}  // namespace fc::ui
