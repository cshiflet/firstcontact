#include "CacheManagerDialog.h"

#include "account/AccountManager.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
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
    resize(720, 460);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(12);

    auto* title = new QLabel(
        tr("<h3 style='margin:0'>Cache storage</h3>"), this);
    title->setTextFormat(Qt::RichText);
    root->addWidget(title);

    auto* hint = new QLabel(tr(
        "Each row shows the on-disk size of the local cache for that "
        "account (cached message bodies + downloaded attachments). "
        "Dropping a cache deletes the rows but keeps the account "
        "signed in — the next sync rebuilds. Orphaned rows are cache "
        "data left behind after an account row was deleted out-of-band."), this);
    hint->setObjectName(QStringLiteral("FormHint"));
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::RichText);
    root->addWidget(hint);

    table_ = new QTableWidget(this);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels({
        tr("Account"), tr("Cached size"),
        tr("Drop cache"), tr("Drop > N days")});
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(
        3, QHeaderView::ResizeToContents);
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

    auto setRow = [&](const QString& id,
                      const QString& display, qint64 size, bool isOrphan) {
        // Column 0: account / orphan label.
        auto* nameItem = new QTableWidgetItem(display);
        if (isOrphan) {
            QFont f = nameItem->font();
            f.setItalic(true);
            nameItem->setFont(f);
        }
        table_->setItem(row, 0, nameItem);

        // Column 1: human-readable size.
        auto* sizeItem = new QTableWidgetItem(humanBytes(size));
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table_->setItem(row, 1, sizeItem);

        // Column 2: Drop cache button.
        auto* dropBtn = new QPushButton(tr("Drop cache"), table_);
        QObject::connect(dropBtn, &QPushButton::clicked, this,
                          [this, id, display] {
            if (QMessageBox::question(this, tr("Drop cache"),
                    tr("Drop cached data for %1?").arg(display))
                    != QMessageBox::Yes) return;
            accounts_->dropCache(id);
            rebuildTable();
        });
        table_->setCellWidget(row, 2, dropBtn);

        // Column 3: Drop > N days.
        auto* purgeBtn = new QPushButton(tr("Drop > N days…"), table_);
        if (isOrphan) purgeBtn->setEnabled(false);   // no point on orphans
        QObject::connect(purgeBtn, &QPushButton::clicked, this,
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
        table_->setCellWidget(row, 3, purgeBtn);

        ++row;
    };

    for (const auto& a : accs) {
        const QString display = a.email.isEmpty()
            ? tr("Unknown account")
            : a.email;
        setRow(a.id, display, accounts_->cacheSizeFor(a.id), false);
    }
    for (const auto& id : orphans) {
        setRow(id, tr("(orphan) %1").arg(id),
               accounts_->cacheSizeFor(id), true);
    }
    table_->setRowCount(row);
}

}  // namespace fc::ui
