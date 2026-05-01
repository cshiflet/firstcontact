#pragma once

#include "models/Message.h"

#include <QMainWindow>
#include <QString>

class QSplitter;
class QLineEdit;

namespace fc { class MessageListModel; }
namespace fc::auth  { class OAuthClient; class ClientConfig; }
namespace fc::api   { class GmailClient; }
namespace fc::sync  { class SyncService; class OutboxWorker;
                      class PendingOpsWorker; class DraftSync; }

namespace fc::ui {

class SidebarWidget;
class MessageListView;
class ReaderPane;
class TrayController;
class Shortcuts;
class ComposeWindow;

// Top-level shell. Owns no services — it gets handed pointers to the
// Bootstrap-managed singletons. All long-lived state lives in cache repos.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(fc::auth::ClientConfig* config,
               fc::auth::OAuthClient* auth,
               fc::api::GmailClient* gmail,
               fc::sync::SyncService* sync,
               fc::sync::OutboxWorker* outbox,
               fc::sync::PendingOpsWorker* pending,
               fc::sync::DraftSync* drafts,
               QWidget* parent = nullptr);

private slots:
    void onSignIn();
    void onSignOut();
    void onRefresh();
    void onLabelSelected(const QString& id);
    void onMessageActivated(const QString& messageId, int row);
    void onSearchChanged();
    void onSearchSubmit();
    void onComposeNew();
    void onReplyCurrent();
    void onReplyAllCurrent();
    void onForwardCurrent();
    void onCreateLabel(const QString& parentLabelId);
    void onRenameLabel(const QString& labelId);
    void onDeleteLabel(const QString& labelId);
    void onToggleStar();
    void onArchiveCurrent();

    void reloadCurrentLabel();
    void reloadSidebar();
    void onNewMessages(int count);

private:
    void buildToolBar();
    void buildLayout();
    void wireSignals();
    void refreshAccountIndicator();
    void openComposeWindow(const fc::Message* parent, int mode);  // mode = ComposeWindow::Mode

    fc::auth::ClientConfig*    config_;
    fc::auth::OAuthClient*     auth_;
    fc::api::GmailClient*      gmail_;
    fc::sync::SyncService*     sync_;
    fc::sync::OutboxWorker*    outbox_;
    fc::sync::PendingOpsWorker* pending_;
    fc::sync::DraftSync*       drafts_;

    QSplitter*               splitter_;
    SidebarWidget*           sidebar_;
    MessageListView*         list_;
    ReaderPane*              reader_;
    fc::MessageListModel*    listModel_;

    QLineEdit*               searchEdit_;
    TrayController*          tray_;
    Shortcuts*               shortcuts_;

    QString                  currentLabelId_;
    QString                  currentSearchQuery_;
    fc::Message              currentMessage_;
    int                      currentRow_ = -1;
};

}  // namespace fc::ui
