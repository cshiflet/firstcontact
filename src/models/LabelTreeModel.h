#pragma once

#include "cache/LabelRepository.h"

#include <QAbstractItemModel>
#include <QHash>
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

private:
    struct Node;
    using NodePtr = std::unique_ptr<Node>;
    Node* root_;
    QString accountId_;

    bool                syncing_ = false;
    QHash<QString, int> shadowUnread_;   // by fullName, survives reload()
    void captureShadow();

    // Fast-path helpers for reload(): if the freshly-queried label
    // rows have the same id set as what's currently in the tree,
    // refreshCountsInPlace updates aggUnread/aggTotal on the existing
    // nodes and emits dataChanged on their Display/Unread/Total roles
    // — no structural reset, no collapsed branches, no dropped
    // selection. sameLabelSet does the cheap pre-check.
    bool sameLabelSet(const std::vector<fc::cache::LabelRow>& rows) const;
    void refreshCountsInPlace(const std::vector<fc::cache::LabelRow>& rows);
};

}  // namespace fc
