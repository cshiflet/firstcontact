#pragma once

#include "Message.h"

#include <QAbstractListModel>

#include <vector>

namespace fc {

// Phase-1: in-memory list of messages backing the centre pane. Phase-2 swaps
// the storage for a SQL-backed model that pages from cache.db.
class MessageListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        ThreadIdRole,
        FromRole,         // pretty: "Name" or addr
        SubjectRole,
        SnippetRole,
        DateRole,         // qint64 ms-epoch
        UnreadRole,
        StarredRole,
        ImportantRole,
        HasAttachmentRole,
        // 0 in non-conversation mode; >0 means this row represents a
        // thread, and the delegate should render a "(N)" count badge.
        ThreadCountRole,
        // QStringList of labelIds carried by this message — the
        // delegate filters down to user labels with assigned colours
        // before rendering.
        LabelIdsRole,
    };

    explicit MessageListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void replaceAll(std::vector<Message> messages);
    const Message* messageAt(int row) const;

private:
    std::vector<Message> rows_;
};

}  // namespace fc
