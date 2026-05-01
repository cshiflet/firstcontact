#include "MainWindow.h"

#include "api/GmailClient.h"
#include "auth/ClientConfig.h"
#include "auth/OAuthClient.h"
#include "cache/Database.h"
#include "cache/LabelRepository.h"
#include "cache/MessageRepository.h"
#include "cache/OutboxRepository.h"
#include "common/Shortcuts.h"
#include "compose/ComposeWindow.h"
#include "messagelist/MessageListView.h"
#include "models/LabelTreeModel.h"
#include "models/MessageListModel.h"
#include "reader/ReaderPane.h"
#include "setup/SetupWizard.h"
#include "sidebar/SidebarWidget.h"
#include "sync/OutboxWorker.h"
#include "sync/SyncService.h"
#include "tray/Notifier.h"
#include "tray/TrayController.h"
#include "util/MimeBuilder.h"

#include <QAction>
#include <QApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>

namespace fc::ui {

namespace {
constexpr int kPageSize = 100;
}

MainWindow::MainWindow(fc::auth::ClientConfig* config,
                       fc::auth::OAuthClient* auth,
                       fc::api::GmailClient* gmail,
                       fc::sync::SyncService* sync,
                       fc::sync::OutboxWorker* outbox,
                       QWidget* parent)
    : QMainWindow(parent),
      config_(config), auth_(auth), gmail_(gmail), sync_(sync), outbox_(outbox) {
    setWindowTitle(QStringLiteral("FirstContact"));
    resize(1200, 760);
    fc::cache::Database::initialize();

    buildLayout();
    buildToolBar();
    wireSignals();

    tray_ = new TrayController(this, this);
    shortcuts_ = new Shortcuts(this);

    statusBar()->showMessage(tr("Ready."));
    refreshAccountIndicator();

    // Hydrate UI from cache without waiting for a network round trip.
    reloadSidebar();
    currentLabelId_ = QStringLiteral("INBOX");
    reloadCurrentLabel();

    // If we have credentials already, kick off background sync.
    if (auth_->isAuthorized()) {
        sync_->runOnce();
        sync_->startScheduler();
        outbox_->start();
    }
}

void MainWindow::buildLayout() {
    splitter_ = new QSplitter(Qt::Horizontal, this);

    sidebar_ = new SidebarWidget(splitter_);
    sidebar_->setMaximumWidth(260);

    list_      = new MessageListView(splitter_);
    listModel_ = new fc::MessageListModel(this);
    list_->setModel(listModel_);

    reader_ = new ReaderPane(splitter_);

    splitter_->addWidget(sidebar_);
    splitter_->addWidget(list_);
    splitter_->addWidget(reader_);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setStretchFactor(2, 2);

    setCentralWidget(splitter_);
}

void MainWindow::buildToolBar() {
    auto* tb = addToolBar(tr("Main"));
    tb->setMovable(false);

    auto* signIn   = tb->addAction(tr("Sign In"));
    auto* refresh  = tb->addAction(tr("Refresh"));
    auto* compose  = tb->addAction(tr("Compose"));
    tb->addSeparator();
    searchEdit_ = new QLineEdit(this);
    searchEdit_->setPlaceholderText(tr("Search mail (operators: from: subject: has:attachment is:unread …)"));
    searchEdit_->setClearButtonEnabled(true);
    tb->addWidget(searchEdit_);
    tb->addSeparator();
    auto* signOut  = tb->addAction(tr("Sign Out"));
    auto* quit     = tb->addAction(tr("Quit"));

    connect(signIn,  &QAction::triggered, this, &MainWindow::onSignIn);
    connect(refresh, &QAction::triggered, this, &MainWindow::onRefresh);
    connect(compose, &QAction::triggered, this, &MainWindow::onComposeNew);
    connect(signOut, &QAction::triggered, this, &MainWindow::onSignOut);
    connect(quit,    &QAction::triggered, qApp, &QApplication::quit);

    connect(searchEdit_, &QLineEdit::returnPressed, this, &MainWindow::onSearchSubmit);
    connect(searchEdit_, &QLineEdit::textChanged,   this, &MainWindow::onSearchChanged);
}

void MainWindow::wireSignals() {
    connect(list_, &MessageListView::messageActivated,
            this,  &MainWindow::onMessageActivated);

    connect(sidebar_, &SidebarWidget::labelSelected,
            this,     &MainWindow::onLabelSelected);
    connect(sidebar_, &SidebarWidget::requestCreateLabel,
            this,     &MainWindow::onCreateLabel);
    connect(sidebar_, &SidebarWidget::requestRenameLabel,
            this,     &MainWindow::onRenameLabel);
    connect(sidebar_, &SidebarWidget::requestDeleteLabel,
            this,     &MainWindow::onDeleteLabel);

    connect(auth_, &fc::auth::OAuthClient::granted, this, [this] {
        refreshAccountIndicator();
        sync_->runOnce();
        sync_->startScheduler();
        outbox_->start();
    });
    connect(auth_, &fc::auth::OAuthClient::failed, this,
            [this](const QString& reason) {
                QMessageBox::warning(this, tr("Sign-in failed"), reason);
            });
    connect(auth_, &fc::auth::OAuthClient::signedOut, this, [this] {
        refreshAccountIndicator();
        listModel_->replaceAll({});
        reader_->showEmpty();
    });

    connect(sync_, &fc::sync::SyncService::labelsUpdated,
            this,  &MainWindow::reloadSidebar);
    connect(sync_, &fc::sync::SyncService::messagesUpdated,
            this,  &MainWindow::reloadCurrentLabel);
    connect(sync_, &fc::sync::SyncService::failed, this,
            [this](const QString& reason) {
                statusBar()->showMessage(tr("Sync error: %1").arg(reason), 5000);
            });
    connect(sync_, &fc::sync::SyncService::newMessages,
            this,  &MainWindow::onNewMessages);

    connect(outbox_, &fc::sync::OutboxWorker::itemSent, this,
            [this](qint64, const QString&) {
                statusBar()->showMessage(tr("Message sent."), 3000);
                sync_->runOnce();
            });
    connect(outbox_, &fc::sync::OutboxWorker::itemFailed, this,
            [this](qint64, const QString& err) {
                statusBar()->showMessage(tr("Send failed: %1").arg(err), 5000);
            });

    if (tray_) {
        connect(tray_, &TrayController::composeRequested,
                this,  &MainWindow::onComposeNew);
        connect(tray_, &TrayController::refreshRequested,
                this,  &MainWindow::onRefresh);
        connect(tray_, &TrayController::quitRequested,
                qApp,  &QApplication::quit);
        connect(tray_->notifier(), &Notifier::openThreadRequested,
                this,  [this](const QString&) {
                    show(); raise(); activateWindow();
                });
    }

    connect(shortcuts_, &Shortcuts::focusSearch,    this, [this]{ searchEdit_->setFocus(); });
    connect(shortcuts_, &Shortcuts::composeNew,     this, &MainWindow::onComposeNew);
    connect(shortcuts_, &Shortcuts::replyToCurrent, this, &MainWindow::onReplyCurrent);
    connect(shortcuts_, &Shortcuts::archiveCurrent, this, &MainWindow::onArchiveCurrent);
    connect(shortcuts_, &Shortcuts::toggleStar,     this, &MainWindow::onToggleStar);
    connect(shortcuts_, &Shortcuts::selectNext, this, [this] {
        const int n = listModel_->rowCount();
        if (n == 0) return;
        const int row = qMin(currentRow_ + 1, n - 1);
        list_->setCurrentIndex(listModel_->index(row, 0));
        onMessageActivated(listModel_->index(row, 0)
                              .data(fc::MessageListModel::IdRole).toString(), row);
    });
    connect(shortcuts_, &Shortcuts::selectPrev, this, [this] {
        if (listModel_->rowCount() == 0) return;
        const int row = qMax(currentRow_ - 1, 0);
        list_->setCurrentIndex(listModel_->index(row, 0));
        onMessageActivated(listModel_->index(row, 0)
                              .data(fc::MessageListModel::IdRole).toString(), row);
    });
    connect(shortcuts_, &Shortcuts::showHelp, this, [this] {
        QMessageBox::information(this, tr("Keyboard shortcuts"),
            tr("/  focus search\n"
               "j  next message\nk  previous message\n"
               "c  compose\nr  reply\ne  archive\n#  delete\n"
               "s  toggle star\n?  this help"));
    });
}

void MainWindow::refreshAccountIndicator() {
    const QString email = auth_->accountEmail();
    setWindowTitle(email.isEmpty()
        ? QStringLiteral("FirstContact")
        : QStringLiteral("FirstContact — %1").arg(email));
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
        sync_->stopScheduler();
        outbox_->stop();
    }
}

void MainWindow::onRefresh() {
    if (!auth_->isAuthorized()) {
        onSignIn();
        return;
    }
    statusBar()->showMessage(tr("Syncing…"));
    sync_->runOnce();
}

void MainWindow::onLabelSelected(const QString& id) {
    if (id.isEmpty() || id == currentLabelId_) return;
    currentLabelId_ = id;
    reloadCurrentLabel();
}

void MainWindow::reloadSidebar() {
    sidebar_->model()->reload();
}

void MainWindow::reloadCurrentLabel() {
    auto rows = currentSearchQuery_.isEmpty()
        ? fc::cache::MessageRepository::listByLabel(currentLabelId_, kPageSize, 0)
        : fc::cache::MessageRepository::searchFts(currentSearchQuery_, kPageSize);
    listModel_->replaceAll(std::move(rows));
    currentRow_ = -1;
    reader_->showEmpty();
    statusBar()->showMessage(currentSearchQuery_.isEmpty()
        ? tr("Showing %1 messages in %2").arg(listModel_->rowCount()).arg(currentLabelId_)
        : tr("Search results: %1").arg(listModel_->rowCount()));
}

void MainWindow::onMessageActivated(const QString& messageId, int row) {
    if (messageId.isEmpty()) return;
    currentRow_ = row;

    fc::Message cached = fc::cache::MessageRepository::byId(messageId);
    if (!cached.id.isEmpty() && !cached.bodyText.isEmpty()) {
        currentMessage_ = cached;
        reader_->showMessage(cached);
        fc::cache::MessageRepository::markAccessed(messageId);
        return;
    }

    reader_->showLoading();
    QPointer<MainWindow> self(this);
    gmail_->getMessage(messageId,
        [self, messageId](fc::Message m, fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                self->reader_->showEmpty(tr("Failed to load: %1").arg(err.message));
                return;
            }
            fc::cache::MessageRepository::upsert(m);
            fc::cache::MessageRepository::markAccessed(messageId);
            self->currentMessage_ = m;
            self->reader_->showMessage(m);
        });
}

void MainWindow::onSearchChanged() {
    // Live search against the local FTS5 index — fast even on large caches.
    currentSearchQuery_ = searchEdit_->text().trimmed();
    reloadCurrentLabel();
}

void MainWindow::onSearchSubmit() {
    // Enter triggers a server-side search using Gmail's `q` syntax to find
    // anything not in the local cache yet, then merge into the model.
    const QString q = searchEdit_->text().trimmed();
    if (q.isEmpty() || !auth_->isAuthorized()) return;
    statusBar()->showMessage(tr("Searching server: %1").arg(q));
    QPointer<MainWindow> self(this);
    gmail_->listMessages({}, q, {}, kPageSize,
        [self, gmail = gmail_, q](fc::api::GmailClient::ListPage page,
                                  fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                self->statusBar()->showMessage(
                    tr("Server search failed: %1").arg(err.message));
                return;
            }
            // Hydrate any missing ids; the cache will then surface them.
            for (const auto& id : page.ids) {
                if (fc::cache::MessageRepository::exists(id)) continue;
                gmail->getMessage(id,
                    [](fc::Message m, fc::api::ApiError gErr) {
                        if (!gErr) fc::cache::MessageRepository::upsert(m);
                    });
            }
            self->statusBar()->showMessage(
                tr("Server returned %1 ids; cache updates in background.")
                    .arg(page.ids.size()));
            self->currentSearchQuery_ = q;
            self->reloadCurrentLabel();
        });
}

void MainWindow::onComposeNew() {
    if (!auth_->isAuthorized()) { onSignIn(); return; }
    auto* w = new ComposeWindow(auth_->accountEmail(), QString(), this);
    w->setAttribute(Qt::WA_DeleteOnClose);
    connect(w, &ComposeWindow::composeReady, this,
        [this](const fc::util::OutgoingMessage& msg, const QString& threadId) {
            const QByteArray rfc = fc::util::MimeBuilder::build(msg);
            fc::cache::OutboxItem item;
            item.rfc5322  = rfc;
            item.threadId = threadId;
            fc::cache::OutboxRepository::enqueue(item);
            outbox_->flush();
        });
    w->show();
}

void MainWindow::onReplyCurrent() {
    if (currentMessage_.id.isEmpty() || !auth_->isAuthorized()) return;
    auto* w = new ComposeWindow(auth_->accountEmail(), QString(), this);
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->prefillFrom(currentMessage_, ComposeWindow::Mode::Reply);
    connect(w, &ComposeWindow::composeReady, this,
        [this](const fc::util::OutgoingMessage& msg, const QString& threadId) {
            const QByteArray rfc = fc::util::MimeBuilder::build(msg);
            fc::cache::OutboxItem item;
            item.rfc5322  = rfc;
            item.threadId = threadId;
            fc::cache::OutboxRepository::enqueue(item);
            outbox_->flush();
        });
    w->show();
}

void MainWindow::onCreateLabel(const QString& parentLabelId) {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("New label"),
        tr("Label name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.isEmpty()) return;

    const auto parent = fc::cache::LabelRepository::byId(parentLabelId);
    if (!parent.id.isEmpty() && parent.type == QLatin1String("user")) {
        name = parent.name + QLatin1Char('/') + name;
    }
    QPointer<MainWindow> self(this);
    gmail_->createLabel(name,
        [self](fc::api::GmailClient::Label l, fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                QMessageBox::warning(self, tr("Create label"), err.message);
                return;
            }
            fc::cache::LabelRow row;
            row.id   = l.id;
            row.name = l.name;
            row.type = l.type;
            fc::cache::LabelRepository::upsert(row);
            self->reloadSidebar();
        });
}

void MainWindow::onRenameLabel(const QString& labelId) {
    const auto current = fc::cache::LabelRepository::byId(labelId);
    if (current.id.isEmpty() || current.type != QLatin1String("user")) return;

    bool ok = false;
    const QString newName = QInputDialog::getText(this, tr("Rename label"),
        tr("New name:"), QLineEdit::Normal, current.name, &ok);
    if (!ok || newName.isEmpty() || newName == current.name) return;

    QPointer<MainWindow> self(this);
    gmail_->updateLabel(labelId, newName,
        [self, labelId, newName](fc::api::GmailClient::Label, fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                QMessageBox::warning(self, tr("Rename label"), err.message);
                return;
            }
            auto row = fc::cache::LabelRepository::byId(labelId);
            row.name = newName;
            fc::cache::LabelRepository::upsert(row);
            self->reloadSidebar();
        });
}

void MainWindow::onDeleteLabel(const QString& labelId) {
    const auto current = fc::cache::LabelRepository::byId(labelId);
    if (current.id.isEmpty() || current.type != QLatin1String("user")) return;
    if (QMessageBox::question(this, tr("Delete label"),
            tr("Delete the label '%1'? Messages keep their other labels.")
              .arg(current.name)) != QMessageBox::Yes) return;

    QPointer<MainWindow> self(this);
    gmail_->deleteLabel(labelId,
        [self, labelId](fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                QMessageBox::warning(self, tr("Delete label"), err.message);
                return;
            }
            fc::cache::LabelRepository::remove(labelId);
            self->reloadSidebar();
        });
}

void MainWindow::onToggleStar() {
    if (currentMessage_.id.isEmpty() || !auth_->isAuthorized()) return;
    const bool wasStarred = currentMessage_.isStarred;
    QStringList add, rem;
    if (wasStarred) rem << QStringLiteral("STARRED");
    else            add << QStringLiteral("STARRED");
    QPointer<MainWindow> self(this);
    gmail_->modifyMessage(currentMessage_.id, add, rem,
        [self, id = currentMessage_.id, add, rem](fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                self->statusBar()->showMessage(
                    tr("Star toggle failed: %1").arg(err.message));
                return;
            }
            fc::cache::MessageRepository::applyLabelDiff(id, add, rem);
            self->reloadCurrentLabel();
        });
}

void MainWindow::onArchiveCurrent() {
    if (currentMessage_.id.isEmpty() || !auth_->isAuthorized()) return;
    QPointer<MainWindow> self(this);
    gmail_->modifyMessage(currentMessage_.id, {}, QStringList{QStringLiteral("INBOX")},
        [self, id = currentMessage_.id](fc::api::ApiError err) {
            if (!self) return;
            if (err) {
                self->statusBar()->showMessage(
                    tr("Archive failed: %1").arg(err.message));
                return;
            }
            fc::cache::MessageRepository::applyLabelDiff(
                id, {}, QStringList{QStringLiteral("INBOX")});
            self->reloadCurrentLabel();
        });
}

void MainWindow::onNewMessages(int count) {
    if (count <= 0 || !tray_ || !tray_->notifier()) return;

    // Pull the most recent message we just upserted to populate the toast.
    auto recent = fc::cache::MessageRepository::listByLabel(
        QStringLiteral("INBOX"), 1, 0);
    QString sender, subject, threadId;
    if (!recent.empty()) {
        sender   = recent.front().fromName.isEmpty()
                     ? recent.front().fromAddr
                     : recent.front().fromName;
        subject  = recent.front().subject;
        threadId = recent.front().threadId;
    }
    tray_->notifier()->notifyNewMail(count, sender, subject, threadId);
}

}  // namespace fc::ui
