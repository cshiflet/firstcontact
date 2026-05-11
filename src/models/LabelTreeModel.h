#pragma once

#include "cache/LabelRepository.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QList>
#include <QString>

#include <memory>
#include <vector>

namespace fc {

// QTreeView model over the cached label set. Gmail labels nest via "/" in
// the name (e.g. "Travel/Booking confirmations"); we materialize that as a
// real tree, keeping system labels at the root in a fixed order.
class LabelTreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        TypeRole,
        UnreadRole,
        TotalRole,
        ColorBgRole,
        ColorFgRole,
        AccountIdRole,
    };

    // Lightweight per-account descriptor — id + display label. fc_models
    // can't depend on fc_account (circular: fc_account -> fc_sync ->
    // fc_models), so callers populate this list from AccountManager and
    // hand it in via setAccounts().
    struct AccountDescriptor {
        QString id;
        QString email;
        QString displayName;   // optional; falls back to email
    };

    explicit LabelTreeModel(QObject* parent = nullptr);
    ~LabelTreeModel() override;

    QModelIndex index(int row, int column,
                      const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void reload();

    // Multi-account tree layout. Pass every signed-in account here
    // (typically from AccountManager::accounts()). reload() builds one
    // top-level branch per account, each containing its own Folders /
    // Labels sections. The "All Inboxes" cross-account synthetic stays
    // at the very top of the tree.
    //
    // When the list is empty the tree falls back to legacy single-
    // account mode: it shows just the Folders / Labels sections of
    // accountId_ (set via setAccountId). v1 startup hits this path
    // briefly while AccountManager populates.
    void setAccounts(const QList<AccountDescriptor>& accounts);
    QList<AccountDescriptor> accounts() const { return accounts_; }

    // Multi-account: switches which account's labels populate the tree.
    // Calling this triggers a reload(). v1: empty string clears the tree.
    void setAccountId(const QString& accountId);
    QString accountId() const;

    // While a sync is active, the unread-count suffix changes from
    // "Inbox (3)" to "Inbox (3…)". Without this hint the suffix
    // briefly disappeared during reload() (beginResetModel wipes the
    // tree, then endResetModel rebuilds it from cache — counts are
    // empty between the two). The shadow map captured before each
    // reload survives the reset, so even if the fresh tree has
    // aggUnread=0 momentarily we still show the user's last
    // known-good count with the ellipsis hint.
    void setSyncing(bool on);

    QString labelIdAt(const QModelIndex& idx) const;
    QString accountIdAt(const QModelIndex& idx) const;

private:
    struct Node;
    using NodePtr = std::unique_ptr<Node>;
    Node* root_;
    QString accountId_;
    QList<AccountDescriptor> accounts_;

    bool                syncing_ = false;
    QHash<QString, int> shadowUnread_;   // by fullName, survives reload()
    void captureShadow();

    // Fast-path helpers for reload(): if the freshly-queried label
    // rows have the same id set as what's currently in the tree,
    // refreshCountsInPlace updates aggUnread/aggTotal on the existing
    // nodes and emits dataChanged on their Display/Unread/Total roles
    // — no structural reset, no collapsed branches, no dropped
    // selection. sameLabelSet does the cheap pre-check. The per-
    // account row list is what reload() collected up front; pass it
    // through so neither helper re-queries the cache.
    using PerAccountRows = std::vector<
        std::pair<QString, std::vector<fc::cache::LabelRow>>>;
    bool sameLabelSet(const PerAccountRows& perAccount) const;
    void refreshCountsInPlace(const PerAccountRows& perAccount);

    // Build the two section nodes (Folders + Labels) for one account
    // under `parent`. Called by reload() for both legacy single-
    // account mode (parent = root_, accountId empty) and multi-
    // account mode (parent = an account node).
    void appendAccountSections(Node* parent,
                                const QString& accountId,
                                const std::vector<fc::cache::LabelRow>& rows);
};

}  // namespace fc
