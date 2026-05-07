#pragma once

#include "Message.h"

#include <QAbstractListModel>
#include <QSet>
#include <QString>

#include <vector>

namespace fc {

// Phase-1: in-memory list of messages backing the centre pane. Phase-2 swaps
// the storage for a SQL-backed model that pages from cache.db.
class MessageListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        // v2: row's source accountId. Used by the unified inbox / cross-
        // account search view so a click can route to the right context.
        AccountIdRole,
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
        // Bool — true if this row was injected as a child of an
        // expanded conversation thread (the delegate paints those
        // indented + dimmer; chevron + count pill are suppressed).
        IsChildRole,
        // Bool — true for parent thread rows whose children are
        // currently shown inline. Drives the chevron direction
        // (right-pointing collapsed, down-pointing expanded).
        IsExpandedRole,
    };

    explicit MessageListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void replaceAll(std::vector<Message> messages);
    const Message* messageAt(int row) const;

    // Toggle inline expansion of the thread at `row`. No-op if the row
    // is already a child or represents a single-message thread. On
    // expand, the thread's older messages (latest-first) are loaded
    // from cache and inserted right after the parent row as children.
    // On collapse, those children are removed in place.
    void toggleThreadExpand(int row);

private:
    std::vector<Message> rows_;
    // Thread ids whose children are currently shown inline. Cleared on
    // every replaceAll — switching label / refresh resets expansion,
    // matching what most desktop mail clients do.
    QSet<QString> expandedThreads_;
};

}  // namespace fc
