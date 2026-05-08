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

bool MessageListModel::canFetchMore(const QModelIndex& parent) const {
    if (parent.isValid()) return false;
    // Search has no offset support in our FTS API (top-K only). Sticking
    // to false for search keeps Qt from poking fetchMore in a loop the
    // model can't actually fulfill.
    if (source_ != Source::ByLabel) return false;
    // The cache is drained for this source — wait for messagesUpdated /
    // a server top-up to bring back new rows. (loadFirstPage / explicit
    // refresh resets this flag.)
    return !cacheDrained_;
}

void MessageListModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid()) return;
    if (source_ != Source::ByLabel) return;
    if (cacheDrained_) return;

    const int offset = static_cast<int>(rows_.size());
    auto more = conversationView_
        ? cache::MessageRepository::listThreadsByLabel(sourceParam_, pageSize(), offset, unreadOnly_)
        : cache::MessageRepository::listByLabel(sourceParam_, pageSize(), offset, unreadOnly_);

    if (more.empty()) {
        cacheDrained_ = true;
        emit cacheExhausted(sourceParam_);
        return;
    }

    const int first = static_cast<int>(rows_.size());
    const int last  = first + static_cast<int>(more.size()) - 1;
    beginInsertRows({}, first, last);
    rows_.insert(rows_.end(),
                 std::make_move_iterator(more.begin()),
                 std::make_move_iterator(more.end()));
    endInsertRows();

    // A short page (less than pageSize) means the cache is on its last
    // legs for this label. Mark drained AND emit cacheExhausted so the
    // owner kicks off a server top-up — Qt's view controllers won't ask
    // for fetchMore again once canFetchMore goes false, so without the
    // signal here the user would be stuck whenever the very last cache
    // page came back short.
    if (static_cast<int>(more.size()) < pageSize()) {
        cacheDrained_ = true;
        emit cacheExhausted(sourceParam_);
    }
}

void MessageListModel::resumeAfterTopUp() {
    // Top-up just finished — there should be more rows in the cache
    // below what we already have loaded. Clear the drain flag and
    // drain the cache into the model in 100-row chunks until we hit
    // either a short page (cache exhausted again — top-up will fire
    // for the next page if user scrolls) or a sane cap, so a single
    // server round-trip's worth of fresh rows lands in the view
    // without forcing the user to scroll-poke for each chunk.
    if (source_ != Source::ByLabel) return;
    cacheDrained_ = false;
    constexpr int kMaxChunksPerResume = 6;   // up to 600 rows per top-up
    for (int i = 0; i < kMaxChunksPerResume && !cacheDrained_; ++i) {
        const int before = static_cast<int>(rows_.size());
        fetchMore({});
        if (static_cast<int>(rows_.size()) == before) break;  // no progress
    }
}

void MessageListModel::setLabelSource(const QString& labelId, bool conversationView,
                                       bool unreadOnly) {
    source_           = Source::ByLabel;
    sourceParam_      = labelId;
    conversationView_ = conversationView;
    unreadOnly_       = unreadOnly;
    loadFirstPage();
}

void MessageListModel::setSearchSource(const QString& query, bool conversationView) {
    source_           = Source::BySearch;
    sourceParam_      = query;
    conversationView_ = conversationView;
    unreadOnly_       = false;   // search results aren't unread-filtered
    loadFirstPage();
}

void MessageListModel::refreshFromSource() {
    if (source_ == Source::None) return;
    // Re-query offset=0 with the current loaded count so a server top-up
    // / incremental sync surfaces without losing the user's scroll
    // window. The loadedRows fallback to pageSize() for the case where
    // we just initialized and rowCount is zero.
    const int limit = qMax(static_cast<int>(rows_.size()), pageSize());

    // Probe one extra row beyond `limit` so we can DETECT whether the
    // cache has more than what we're about to show. Without the probe,
    // a refresh that lands rows_.size() == limit looks identical
    // whether the cache has exactly `limit` rows total (drained) or
    // millions more (lots more to scroll). That ambiguity led to a
    // hot loop:
    //   fetchMore drains -> cacheExhausted -> topUpLabel (no missing
    //   because we've walked the whole label) -> messagesUpdated ->
    //   refreshFromSource resets cacheDrained_ to false ->
    //   fetchMore drains again -> ...
    // The probe distinguishes the two cases cleanly: "rows.size() >
    // limit" means there's more, "rows.size() == limit (or less)"
    // means we've reached the end.
    const int probe = limit + 1;

    std::vector<Message> rows;
    bool moreInCache = false;
    if (source_ == Source::ByLabel) {
        rows = conversationView_
            ? cache::MessageRepository::listThreadsByLabel(sourceParam_, probe, 0, unreadOnly_)
            : cache::MessageRepository::listByLabel(sourceParam_, probe, 0, unreadOnly_);
        moreInCache = static_cast<int>(rows.size()) > limit;
        if (moreInCache) rows.pop_back();   // we only show `limit` rows
    } else {
        // Search has no offset support, so the probe doesn't apply —
        // FTS top-K returns however many results match, capped at
        // `limit`.
        rows = conversationView_
            ? cache::MessageRepository::searchFtsThreads(sourceParam_, limit)
            : cache::MessageRepository::searchFts(sourceParam_, limit);
    }

    beginResetModel();
    rows_ = std::move(rows);
    expandedThreads_.clear();
    cacheDrained_ = (source_ == Source::BySearch) || !moreInCache;
    endResetModel();
}

void MessageListModel::loadFirstPage() {
    std::vector<Message> rows;
    if (source_ == Source::ByLabel) {
        rows = conversationView_
            ? cache::MessageRepository::listThreadsByLabel(sourceParam_, pageSize(), 0, unreadOnly_)
            : cache::MessageRepository::listByLabel(sourceParam_, pageSize(), 0, unreadOnly_);
    } else if (source_ == Source::BySearch) {
        rows = conversationView_
            ? cache::MessageRepository::searchFtsThreads(sourceParam_, pageSize())
            : cache::MessageRepository::searchFts(sourceParam_, pageSize());
    }

    beginResetModel();
    rows_ = std::move(rows);
    expandedThreads_.clear();
    cacheDrained_ = (source_ == Source::BySearch)
        || static_cast<int>(rows_.size()) < pageSize();
    endResetModel();
}

QString MessageListModel::sourceLabelId() const {
    return source_ == Source::ByLabel ? sourceParam_ : QString();
}

void MessageListModel::replaceAll(std::vector<Message> messages) {
    beginResetModel();
    rows_ = std::move(messages);
    expandedThreads_.clear();   // expansion state doesn't survive a reload
    // External replaceAll can't be paginated — caller is in charge of
    // what's in the model. Keep canFetchMore happy by treating this as
    // drained.
    source_       = Source::None;
    sourceParam_.clear();
    cacheDrained_ = true;
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
