#include "CacheManagerDialog.h"

#include "account/AccountManager.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QVBoxLayout>

namespace fc::ui {

namespace {

QString humanBytes(qint64 b) {
    constexpr qint64 KB = 1024;
    constexpr qint64 MB = KB * 1024;
    constexpr qint64 GB = MB * 1024;
    if (b >= GB) return QStringLiteral("%1 GB").arg(double(b) / GB, 0, 'f', 2);
    if (b >= MB) return QStringLiteral("%1 MB").arg(double(b) / MB, 0, 'f', 1);
    if (b >= KB) return QStringLiteral("%1 KB").arg(double(b) / KB, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(b);
}

}  // namespace

CacheManagerDialog::CacheManagerDialog(
        fc::account::AccountManager* accounts, QWidget* parent)
    : QDialog(parent), accounts_(accounts) {
    setWindowTitle(tr("Cache storage"));
    resize(880, 520);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(12);

    auto* title = new QLabel(
        tr("<h3 style='margin:0'>Cache storage</h3>"), this);
    title->setTextFormat(Qt::RichText);
    root->addWidget(title);

    auto* hint = new QLabel(tr(
        "Each row shows the on-disk cache footprint for an account "
        "(message bodies, metadata, downloaded attachments). Dropping "
        "a cache deletes the rows but keeps the account signed in — "
        "the next sync rebuilds. <b>Orphaned</b> rows are cache data "
        "left over after an account row was deleted out-of-band."), this);
    hint->setObjectName(QStringLiteral("FormHint"));
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::RichText);
    root->addWidget(hint);

    // Totals strip — sums every signed-in account's cache footprint
    // and counts. Mirrors the per-row columns so the user can see the
    // aggregate at a glance.
    totalLabel_ = new QLabel(this);
    totalLabel_->setObjectName(QStringLiteral("FormHint"));
    totalLabel_->setTextFormat(Qt::RichText);
    totalLabel_->setWordWrap(true);
    root->addWidget(totalLabel_);

    table_ = new QTableWidget(this);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setColumnCount(5);
    table_->setHorizontalHeaderLabels({
        tr("Account"),
        tr("Messages"),
        tr("Folders"),
        tr("Cached size"),
        tr("Actions")});
    auto* hdr = table_->horizontalHeader();
    hdr->setStretchLastSection(false);
    hdr->setSectionResizeMode(0, QHeaderView::Stretch);
    hdr->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table_->verticalHeader()->setVisible(false);
    root->addWidget(table_, /*stretch=*/1);

    rebuildTable();

    auto* btnRow = new QHBoxLayout;
    auto* clearAllBtn = new QPushButton(tr("Clear all caches"), this);
    auto* dropOrphansBtn = new QPushButton(tr("Drop orphaned cache"), this);
    auto* closeBtn = new QPushButton(tr("Close"), this);
    closeBtn->setObjectName(QStringLiteral("primary"));
    closeBtn->setDefault(true);
    btnRow->addWidget(clearAllBtn);
    btnRow->addWidget(dropOrphansBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    connect(clearAllBtn, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Clear all caches"),
                tr("Drop the local cache for every account? Each "
                   "account stays signed in; the next sync will "
                   "re-download.")) != QMessageBox::Yes) return;
        for (const auto& a : accounts_->accounts()) {
            accounts_->dropCache(a.id);
        }
        rebuildTable();
    });

    connect(dropOrphansBtn, &QPushButton::clicked, this, [this] {
        const auto orphans = accounts_->orphanedAccountIds();
        if (orphans.isEmpty()) {
            QMessageBox::information(this, tr("Drop orphaned cache"),
                tr("No orphaned cache rows."));
            return;
        }
        if (QMessageBox::question(this, tr("Drop orphaned cache"),
                tr("Found %1 orphaned account(s). Drop their cache?")
                    .arg(orphans.size())) != QMessageBox::Yes) return;
        const int n = accounts_->dropOrphanedCache();
        QMessageBox::information(this, tr("Drop orphaned cache"),
            tr("Cleaned up %1 orphan(s).").arg(n));
        rebuildTable();
    });

    if (accounts_) {
        connect(accounts_, &fc::account::AccountManager::accountsChanged,
                this, &CacheManagerDialog::rebuildTable);
    }
}

void CacheManagerDialog::rebuild() { rebuildTable(); }

void CacheManagerDialog::rebuildTable() {
    if (!table_ || !accounts_) return;
    table_->setRowCount(0);

    const auto accs = accounts_->accounts();
    const auto orphans = accounts_->orphanedAccountIds();

    int row = 0;
    table_->setRowCount(accs.size() + orphans.size());

    qint64 totalBytes = 0;
    int totalMessages = 0, totalFolders = 0, totalThreads = 0;
    int totalAttachments = 0, totalDrafts = 0;

    auto setRow = [&](const QString& id,
                      const QString& display, bool isOrphan) {
        const auto stats = accounts_->statsFor(id);
        if (!isOrphan) {
            totalBytes      += stats.sizeBytes;
            totalMessages   += stats.messageCount;
            totalFolders    += stats.labelCount;
            totalThreads    += stats.threadCount;
            totalAttachments+= stats.attachmentCount;
            totalDrafts     += stats.draftCount;
        }

        // Column 0: account / orphan label, with a tooltip exposing
        // the full per-account stats so power users can see threads /
        // attachments / drafts / outbox / pending_ops without us
        // splattering more columns into the table.
        auto* nameItem = new QTableWidgetItem(display);
        if (isOrphan) {
            QFont f = nameItem->font();
            f.setItalic(true);
            nameItem->setFont(f);
        }
        nameItem->setToolTip(tr(
            "Messages: %1\nThreads: %2\nFolders: %3\nAttachments: %4\n"
            "Drafts: %5\nOutbox: %6\nPending ops: %7")
            .arg(stats.messageCount)
            .arg(stats.threadCount)
            .arg(stats.labelCount)
            .arg(stats.attachmentCount)
            .arg(stats.draftCount)
            .arg(stats.outboxCount)
            .arg(stats.pendingOpsCount));
        table_->setItem(row, 0, nameItem);

        auto* msgItem = new QTableWidgetItem(
            QLocale::system().toString(stats.messageCount));
        msgItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table_->setItem(row, 1, msgItem);

        auto* lblItem = new QTableWidgetItem(
            QLocale::system().toString(stats.labelCount));
        lblItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table_->setItem(row, 2, lblItem);

        auto* sizeItem = new QTableWidgetItem(humanBytes(stats.sizeBytes));
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table_->setItem(row, 3, sizeItem);

        // Column 4: a single "Manage…" button that drops a menu of
        // every per-account action. Saves horizontal space and lets
        // us add more actions later without growing the table.
        auto* manageBtn = new QToolButton(table_);
        manageBtn->setText(tr("Manage…"));
        manageBtn->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(manageBtn);

        auto* dropAct = menu->addAction(tr("Drop entire cache"));
        QObject::connect(dropAct, &QAction::triggered, this,
                          [this, id, display] {
            if (QMessageBox::question(this, tr("Drop cache"),
                    tr("Drop cached data for %1?").arg(display))
                    != QMessageBox::Yes) return;
            accounts_->dropCache(id);
            rebuildTable();
        });

        auto* olderAct = menu->addAction(tr("Drop messages older than…"));
        if (isOrphan) olderAct->setEnabled(false);
        QObject::connect(olderAct, &QAction::triggered, this,
                          [this, id] {
            bool ok = false;
            const int days = QInputDialog::getInt(this,
                tr("Drop messages older than"),
                tr("Days:"), 30, 1, 3650, 1, &ok);
            if (!ok) return;
            const int n = accounts_->clearMessagesOlderThan(id, days);
            QMessageBox::information(this, tr("Drop messages"),
                tr("Deleted %1 message(s) older than %2 day(s).")
                    .arg(n).arg(days));
            rebuildTable();
        });

        auto* reduceAct = menu->addAction(tr("Reduce cache to size…"));
        if (isOrphan) reduceAct->setEnabled(false);
        QObject::connect(reduceAct, &QAction::triggered, this,
                          [this, id, display] {
            bool ok = false;
            // Megabyte input — gigabytes feel wrong for typical
            // mailbox sizes, kilobytes are too granular. 5 MB floor
            // so users can't accidentally reduce-to-zero (use Drop
            // entire cache for that).
            const int mb = QInputDialog::getInt(this,
                tr("Reduce cache to"),
                tr("Target size (MB):"), 200, 5, 100000, 50, &ok);
            if (!ok) return;
            const qint64 target = qint64(mb) * 1024LL * 1024LL;
            const int n = accounts_->clearMessagesToTargetSize(id, target);
            QMessageBox::information(this, tr("Reduce cache"),
                tr("Deleted %1 oldest message(s) from %2 to fit "
                   "%3 MB target.")
                    .arg(n).arg(display).arg(mb));
            rebuildTable();
        });

        manageBtn->setMenu(menu);
        table_->setCellWidget(row, 4, manageBtn);

        ++row;
    };

    for (const auto& a : accs) {
        const QString display = a.email.isEmpty()
            ? tr("Unknown account")
            : a.email;
        setRow(a.id, display, false);
    }
    for (const auto& id : orphans) {
        setRow(id, tr("(orphan) %1").arg(id), true);
    }
    table_->setRowCount(row);

    if (totalLabel_) {
        totalLabel_->setText(tr(
            "<b>Totals (signed-in accounts):</b> "
            "%1 — %2 messages • %3 threads • %4 folders • "
            "%5 attachments • %6 drafts")
            .arg(humanBytes(totalBytes))
            .arg(QLocale::system().toString(totalMessages))
            .arg(QLocale::system().toString(totalThreads))
            .arg(QLocale::system().toString(totalFolders))
            .arg(QLocale::system().toString(totalAttachments))
            .arg(QLocale::system().toString(totalDrafts)));
    }
}

}  // namespace fc::ui
