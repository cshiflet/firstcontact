#pragma once

#include "models/Message.h"

#include <QMainWindow>

class QSplitter;
class QListWidget;
class QStatusBar;

namespace fc { class MessageListModel; }
namespace fc::auth { class OAuthClient; class ClientConfig; }
namespace fc::api  { class GmailClient; }

namespace fc::ui {

class MessageListView;
class ReaderPane;

// Three-pane shell: sidebar (Phase-1 stub), message list, reader. Owns no
// services; gets handed pointers to the wired-up app.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(fc::auth::ClientConfig* config,
               fc::auth::OAuthClient* auth,
               fc::api::GmailClient* gmail,
               QWidget* parent = nullptr);

private slots:
    void onSignIn();
    void onRefresh();
    void onMessageActivated(const QString& messageId, int row);
    void onSignOut();

private:
    void buildToolBar();
    void buildLayout();
    void refreshAccountIndicator();

    fc::auth::ClientConfig* config_;
    fc::auth::OAuthClient*  auth_;
    fc::api::GmailClient*   gmail_;

    QSplitter*              splitter_;
    QListWidget*            sidebar_;
    MessageListView*        list_;
    ReaderPane*             reader_;
    fc::MessageListModel*   model_;
};

}  // namespace fc::ui
