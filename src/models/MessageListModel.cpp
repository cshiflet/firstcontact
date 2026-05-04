#include "MessageListModel.h"

#include "cache/MessageRepository.h"
#include "util/Html2Text.h"

#include <algorithm>

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
        case LabelIdsRole:      return m.labelIds;
        case IsChildRole:       return m.isThreadChild;
        case IsExpandedRole:    return !m.isThreadChild
                                     && expandedThreads_.contains(m.threadId);
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
        {LabelIdsRole,      "labelIds"},
        {IsChildRole,       "isChild"},
        {IsExpandedRole,    "expanded"},
    };
}

void MessageListModel::replaceAll(std::vector<Message> messages) {
    beginResetModel();
    rows_ = std::move(messages);
    expandedThreads_.clear();   // expansion state doesn't survive a reload
    endResetModel();
}

const Message* MessageListModel::messageAt(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
    return &rows_[row];
}

void MessageListModel::toggleThreadExpand(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const Message parent = rows_[row];
    if (parent.isThreadChild)        return;   // children themselves don't expand
    if (parent.threadCount <= 1)     return;   // nothing to expand
    if (parent.threadId.isEmpty())   return;

    if (expandedThreads_.contains(parent.threadId)) {
        // Collapse: walk forward from row+1 while we're still inside
        // children of THIS thread, then remove them in one go.
        int last = row;
        for (size_t i = static_cast<size_t>(row) + 1; i < rows_.size(); ++i) {
            if (!rows_[i].isThreadChild) break;
            if (rows_[i].threadId != parent.threadId) break;
            last = static_cast<int>(i);
        }
        if (last > row) {
            beginRemoveRows({}, row + 1, last);
            rows_.erase(rows_.begin() + row + 1, rows_.begin() + last + 1);
            endRemoveRows();
        }
        expandedThreads_.remove(parent.threadId);
        // Refresh the parent so the chevron flips direction.
        const auto idx = index(row, 0);
        emit dataChanged(idx, idx, {IsExpandedRole});
        return;
    }

    // Expand: load the entire thread and inject every message OTHER
    // than the parent (the parent row already represents the latest
    // message, see MessageRepository::listThreadsByLabel). Sort newest-
    // first so children read top-to-bottom from most-recent reply
    // backwards through the conversation.
    auto thread = fc::cache::MessageRepository::byThread(parent.threadId);
    std::sort(thread.begin(), thread.end(),
              [](const Message& a, const Message& b) {
                  return a.internalDate > b.internalDate;
              });
    std::vector<Message> children;
    children.reserve(thread.size());
    for (auto& m : thread) {
        if (m.id == parent.id) continue;       // parent IS the latest
        m.isThreadChild = true;
        m.threadCount   = 0;                   // suppress the count pill
        children.push_back(std::move(m));
    }
    if (children.empty()) {
        // Cache disagrees with the threadCount aggregate (e.g., one of
        // the messages got dropped by the LRU evictor). Mark expanded
        // anyway so the chevron flips, but inject nothing.
        expandedThreads_.insert(parent.threadId);
        const auto idx = index(row, 0);
        emit dataChanged(idx, idx, {IsExpandedRole});
        return;
    }
    beginInsertRows({}, row + 1,
                    row + static_cast<int>(children.size()));
    rows_.insert(rows_.begin() + row + 1,
                 std::make_move_iterator(children.begin()),
                 std::make_move_iterator(children.end()));
    endInsertRows();
    expandedThreads_.insert(parent.threadId);
    const auto idx = index(row, 0);
    emit dataChanged(idx, idx, {IsExpandedRole});
}

}  // namespace fc
