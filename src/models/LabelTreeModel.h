#pragma once

#include "cache/LabelRepository.h"

#include <QAbstractItemModel>

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

    QString labelIdAt(const QModelIndex& idx) const;

private:
    struct Node;
    using NodePtr = std::unique_ptr<Node>;
    Node* root_;
    QString accountId_;
};

}  // namespace fc
