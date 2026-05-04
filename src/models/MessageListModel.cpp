#include "MessageListModel.h"

#include "util/Html2Text.h"

namespace fc {

MessageListModel::MessageListModel(QObject* parent) : QAbstractListModel(parent) {}

int MessageListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant MessageListModel::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid() || idx.row() < 0 ||
        idx.row() >= static_cast<int>(rows_.size())) return {};
    const Message& m = rows_[idx.row()];

    // Belt-and-braces decode for rows that were cached before MessageParser
    // started decoding entities at ingest. The decoder is idempotent and
    // skips strings without an '&' so the cost is negligible.
    auto decode = [](const QString& s) { return fc::util::decodeHtmlEntities(s); };

    switch (role) {
        case IdRole:            return m.id;
        case ThreadIdRole:      return m.threadId;
        case FromRole:          return decode(
                                     m.fromName.isEmpty() ? m.fromAddr : m.fromName);
        case SubjectRole:       return decode(m.subject);
        case SnippetRole:       return decode(m.snippet);
        case DateRole:          return m.internalDate;
        case UnreadRole:        return m.isUnread;
        case StarredRole:       return m.isStarred;
        case ImportantRole:     return m.isImportant;
        case HasAttachmentRole: return m.hasAttachment;
        case ThreadCountRole:   return m.threadCount;
        case Qt::DisplayRole:
            return QStringLiteral("%1 — %2").arg(
                decode(m.fromName.isEmpty() ? m.fromAddr : m.fromName),
                decode(m.subject));
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
        {ThreadCountRole,   "threadCount"},
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
