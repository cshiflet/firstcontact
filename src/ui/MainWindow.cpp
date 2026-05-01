#include "MainWindow.h"

#include "api/GmailClient.h"
#include "auth/ClientConfig.h"
#include "auth/OAuthClient.h"
#include "messagelist/MessageListView.h"
#include "models/MessageListModel.h"
#include "reader/ReaderPane.h"
#include "setup/SetupWizard.h"

#include <QAction>
#include <QApplication>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

namespace fc::ui {

namespace {
constexpr int kInboxFetch = 25;
constexpr int kBatchOpenLimit = 10;  // Phase-1: serial gets after listing
}

MainWindow::MainWindow(fc::auth::ClientConfig* config,
                       fc::auth::OAuthClient* auth,
                       fc::api::GmailClient* gmail,
                       QWidget* parent)
    : QMainWindow(parent), config_(config), auth_(auth), gmail_(gmail) {
    setWindowTitle(QStringLiteral("FirstContact"));
    resize(1100, 720);

    buildLayout();
    buildToolBar();

    statusBar()->showMessage(tr("Ready."));
    refreshAccountIndicator();

    connect(auth_, &fc::auth::OAuthClient::granted, this, [this] {
        refreshAccountIndicator();
        onRefresh();
    });
    connect(auth_, &fc::auth::OAuthClient::failed, this,
            [this](const QString& reason) {
                QMessageBox::warning(this, tr("Sign-in failed"), reason);
            });
    connect(auth_, &fc::auth::OAuthClient::signedOut, this,
            [this] { refreshAccountIndicator(); model_->replaceAll({}); reader_->showEmpty(); });
}

void MainWindow::buildLayout() {
    splitter_ = new QSplitter(Qt::Horizontal, this);

    sidebar_ = new QListWidget(splitter_);
    sidebar_->addItems({QStringLiteral("Inbox"),
                        QStringLiteral("Starred"),
                        QStringLiteral("Sent"),
                        QStringLiteral("Drafts"),
                        QStringLiteral("All Mail"),
                        QStringLiteral("Spam"),
                        QStringLiteral("Trash")});
    sidebar_->setCurrentRow(0);
    sidebar_->setMaximumWidth(220);

    list_ = new MessageListView(splitter_);
    model_ = new fc::MessageListModel(this);
    list_->setModel(model_);

    reader_ = new ReaderPane(splitter_);

    splitter_->addWidget(sidebar_);
    splitter_->addWidget(list_);
    splitter_->addWidget(reader_);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setStretchFactor(2, 2);

    setCentralWidget(splitter_);

    connect(list_, &MessageListView::messageActivated,
            this, &MainWindow::onMessageActivated);
}

void MainWindow::buildToolBar() {
    auto* tb = addToolBar(tr("Main"));
    tb->setMovable(false);

    auto* signIn = tb->addAction(tr("Sign In"));
    connect(signIn, &QAction::triggered, this, &MainWindow::onSignIn);

    auto* refresh = tb->addAction(tr("Refresh"));
    connect(refresh, &QAction::triggered, this, &MainWindow::onRefresh);

    tb->addSeparator();

    auto* signOut = tb->addAction(tr("Sign Out"));
    connect(signOut, &QAction::triggered, this, &MainWindow::onSignOut);

    auto* quit = tb->addAction(tr("Quit"));
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);
}

void MainWindow::refreshAccountIndicator() {
    const QString email = auth_->accountEmail();
    statusBar()->showMessage(email.isEmpty()
        ? tr("Not signed in.")
        : tr("Signed in as %1").arg(email));
}

void MainWindow::onSignIn() {
    if (!config_->isConfigured()) {
        SetupWizard wiz(config_, this);
        if (wiz.exec() != QDialog::Accepted) return;
    }
    auth_->authorize();
}

void MainWindow::onSignOut() {
    if (QMessageBox::question(this, tr("Sign out"),
                              tr("Sign out and clear cached credentials?"))
            == QMessageBox::Yes) {
        auth_->signOut();
    }
}

void MainWindow::onRefresh() {
    if (!auth_->isAuthorized()) {
        QMessageBox::information(this, tr("Not signed in"),
            tr("Use Sign In first."));
        return;
    }

    statusBar()->showMessage(tr("Loading inbox…"));
    reader_->showEmpty(tr("Loading inbox…"));
    model_->replaceAll({});

    gmail_->listMessages(QStringLiteral("INBOX"), {}, {}, kInboxFetch,
        [this](fc::api::GmailClient::ListPage page, fc::api::ApiError err) {
            if (err) {
                statusBar()->showMessage(tr("List failed: %1").arg(err.message));
                return;
            }
            if (page.ids.isEmpty()) {
                statusBar()->showMessage(tr("Inbox empty."));
                return;
            }

            // Phase-1: serial fetch up to kBatchOpenLimit message bodies for
            // a populated list. Phase-2 replaces this with a real batch +
            // SQLite-backed model that pages incrementally.
            auto results = std::make_shared<std::vector<fc::Message>>();
            auto remaining = std::make_shared<int>(
                std::min<int>(int(page.ids.size()), kBatchOpenLimit));
            results->reserve(*remaining);

            for (int i = 0; i < *remaining; ++i) {
                gmail_->getMessage(page.ids.at(i),
                    [this, results, remaining]
                    (fc::Message m, fc::api::ApiError gErr) {
                        if (!gErr) results->push_back(std::move(m));
                        if (--(*remaining) > 0) return;
                        std::sort(results->begin(), results->end(),
                            [](const fc::Message& a, const fc::Message& b) {
                                return a.internalDate > b.internalDate;
                            });
                        model_->replaceAll(*results);
                        statusBar()->showMessage(
                            tr("Loaded %1 messages.").arg(results->size()));
                    });
            }
        });
}

void MainWindow::onMessageActivated(const QString& messageId, int row) {
    if (const fc::Message* cached = model_->messageAt(row); cached && !cached->bodyText.isEmpty()) {
        reader_->showMessage(*cached);
        return;
    }
    reader_->showLoading();
    gmail_->getMessage(messageId,
        [this](fc::Message m, fc::api::ApiError err) {
            if (err) {
                reader_->showEmpty(tr("Failed to load: %1").arg(err.message));
                return;
            }
            reader_->showMessage(m);
        });
}

}  // namespace fc::ui
