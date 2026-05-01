#include "MessageListModel.h"

namespace fc {

MessageListModel::MessageListModel(QObject* parent) : QAbstractListModel(parent) {}

int MessageListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant MessageListModel::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid() || idx.row() < 0 ||
        idx.row() >= static_cast<int>(rows_.size())) return {};
    const Message& m = rows_[idx.row()];

    switch (role) {
        case IdRole:            return m.id;
        case ThreadIdRole:      return m.threadId;
        case FromRole:          return m.fromName.isEmpty() ? m.fromAddr : m.fromName;
        case SubjectRole:       return m.subject;
        case SnippetRole:       return m.snippet;
        case DateRole:          return m.internalDate;
        case UnreadRole:        return m.isUnread;
        case StarredRole:       return m.isStarred;
        case ImportantRole:     return m.isImportant;
        case HasAttachmentRole: return m.hasAttachment;
        case Qt::DisplayRole:
            return QStringLiteral("%1 — %2").arg(
                m.fromName.isEmpty() ? m.fromAddr : m.fromName, m.subject);
        default:                return {};
    }
}

QHash<int, QByteArray> MessageListModel::roleNames() const {
    return {
        {IdRole,            "id"},
        {ThreadIdRole,      "threadId"},
        {FromRole,          "from"},
        {SubjectRole,       "subject"},
        {SnippetRole,       "snippet"},
        {DateRole,          "date"},
        {UnreadRole,        "unread"},
        {StarredRole,       "starred"},
        {ImportantRole,     "important"},
        {HasAttachmentRole, "hasAttachment"},
    };
}

void MessageListModel::replaceAll(std::vector<Message> messages) {
    beginResetModel();
    rows_ = std::move(messages);
    endResetModel();
}

const Message* MessageListModel::messageAt(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
    return &rows_[row];
}

}  // namespace fc
