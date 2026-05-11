#pragma once

#include <QSet>
#include <QWidget>

class QTreeView;

namespace fc { class LabelTreeModel; }
namespace fc::account { class AccountManager; }

namespace fc::ui {

// Left pane: QTreeView over LabelTreeModel. Renders one branch per
// signed-in account (with Folders / Labels children) plus a cross-
// account "All Inboxes" at the top. Emits account-aware signals so
// MainWindow can switch the active account + label in one step.
class SidebarWidget : public QWidget {
    Q_OBJECT
public:
    explicit SidebarWidget(QWidget* parent = nullptr);

    // Bind to AccountManager. SidebarWidget listens to accountsChanged
    // and pushes the current account list into LabelTreeModel; it also
    // listens to currentAccountChanged so the "active focus" hint can
    // track the toolbar's account switcher.
    void setAccountManager(fc::account::AccountManager* accounts);

    fc::LabelTreeModel* model() const;
    void selectLabel(const QString& accountId, const QString& labelId);
    // Cheap repaint with no data change — used after a Settings toggle
    // flips a delegate-time preference (e.g. label-colour swatches).
    void refreshAppearance();

signals:
    // Account-aware. accountId is empty for the cross-account "All
    // Inboxes" synthetic; otherwise it identifies which account the
    // labelId belongs to. MainWindow uses this to switch the active
    // account if needed before navigating to the label.
    void labelSelected(const QString& accountId, const QString& labelId);
    void requestCreateLabel(const QString& accountId,
                            const QString& parentLabelId);
    void requestRenameLabel(const QString& accountId, const QString& labelId);
    void requestDeleteLabel(const QString& accountId, const QString& labelId);
    // Right-click → "Cache all messages in this label". MainWindow
    // owns the workflow (confirmation dialog, status-bar feedback,
    // SyncService::cacheLabelComplete call).
    void requestCacheLabel(const QString& accountId, const QString& labelId);

private slots:
    void onClicked(const QModelIndex& idx);
    void onContextMenu(const QPoint& p);

private:
    void pushAccountsToModel();
    QString expandKey(const QModelIndex& idx) const;
    void saveExpandState();
    void restoreExpandState();
    void expandPathTo(const QString& accountId, const QString& labelId);

    QTreeView*           tree_;
    fc::LabelTreeModel*  model_;
    fc::account::AccountManager* accounts_ = nullptr;
    // Cached selection (accountId, labelId). Re-applied after every
    // modelReset because Qt drops currentIndex through reset events.
    QString              selectedAccountId_;
    QString              selectedLabelId_;
    // Persisted expand state. expanded_ holds the QSettings-stored
    // keys ("__all_inboxes", "__account/<aid>", "__section/<aid>/...",
    // or "<aid>\t<labelFullName>"). Mutated when the user
    // expands/collapses; persisted on every change.
    QSet<QString>        expanded_;
    bool                 restoringExpand_ = false;
};

}  // namespace fc::ui
