#pragma once

#include <QDialog>

class QLabel;
class QTableWidget;

namespace fc::account { class AccountManager; }

namespace fc::ui {

// v4 — cache manager. A table view showing every account's cached size
// on disk plus an "Orphaned" group for stale account_ids whose
// accounts row no longer exists. Actions:
//   - Drop this account's cache (per-row button).
//   - Drop messages older than N days (per-row button).
//   - Drop orphaned cache (footer button, only when orphans present).
//   - Clear all (footer; nukes every per-account cache row, keeps
//     accounts rows).
class CacheManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit CacheManagerDialog(fc::account::AccountManager* accounts,
                                 QWidget* parent = nullptr);

private:
    void rebuild();
    void rebuildTable();

    fc::account::AccountManager* accounts_;
    QTableWidget* table_ = nullptr;
    QLabel*       totalLabel_ = nullptr;
};

}  // namespace fc::ui
