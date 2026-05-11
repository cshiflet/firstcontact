#include "MessageListModel.h"

#include "cache/MessageRepository.h"
#include "util/Html2Text.h"

#include <QScopedValueRollback>

#include <algorithm>

namespace fc {

MessageListModel::MessageListModel(QObject* parent) : QAbstractListModel(parent) {}

int MessageListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(rows_.size())
         + (footerState_ != FooterState::None ? 1 : 0);
}

Qt::ItemFlags MessageListModel::flags(const QModelIndex& idx) const {
    if (!idx.isValid()) return Qt::NoItemFlags;
    const int row = idx.row();
    if (row >= static_cast<int>(rows_.size())) {
        // Placeholder footer row — visible only, not interactable.
        return Qt::ItemIsEnabled;
    }
    return QAbstractListModel::flags(idx);
}

QVariant MessageListModel::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid() || idx.row() < 0) return {};
    const int row = idx.row();
    // Synthetic footer row (Loading more… / No more messages).
    if (row >= static_cast<int>(rows_.size())) {
        if (footerState_ == FooterState::None) return {};
        const QString text = (footerState_ == FooterState::Loading)
            ? QStringLiteral("Loading more messages…")
            : QStringLiteral("No more messages");
        switch (role) {
            case IsPlaceholderRole:    return true;
            case PlaceholderTextRole:  return text;
            case Qt::DisplayRole:      return text;
            default:                   return {};
        }
    }
    const Message& m = rows_[row];

    // Belt-and-braces decode for rows that were cached before MessageParser
    // started decoding entities at ingest. The decoder is idempotent and
    // skips strings without an '&' so the cost is negligible.
    auto decode = [](const QString& s) { return fc::util::decodeHtmlEntities(s); };

    switch (role) {
        case IdRole:            return m.id;
        case AccountIdRole:     return m.accountId;
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
        {IdRole,                "id"},
        {AccountIdRole,         "accountId"},
        {ThreadIdRole,          "threadId"},
        {FromRole,              "from"},
        {SubjectRole,           "subject"},
        {SnippetRole,           "snippet"},
        {DateRole,              "date"},
        {UnreadRole,            "unread"},
        {StarredRole,           "starred"},
        {ImportantRole,         "important"},
        {HasAttachmentRole,     "hasAttachment"},
        {ThreadCountRole,       "threadCount"},
        {LabelIdsRole,          "labelIds"},
        {IsChildRole,           "isChild"},
        {IsExpandedRole,        "expanded"},
        {IsPlaceholderRole,     "isPlaceholder"},
        {PlaceholderTextRole,   "placeholderText"},
    };
}

void MessageListModel::setFooterState(FooterState s) {
    const char* names[] = {"None", "Loading", "NoMore"};
    qInfo("MessageListModel::setFooterState %s -> %s (realRows=%lld)",
          names[static_cast<int>(footerState_)],
          names[static_cast<int>(s)],
          static_cast<long long>(rows_.size()));
    if (footerState_ == s) return;
    const int realRows = static_cast<int>(rows_.size());
    const bool wasShowing = footerState_ != FooterState::None;
    const bool willShow   = s != FooterState::None;
    if (wasShowing && !willShow) {
        beginRemoveRows({}, realRows, realRows);
        footerState_ = s;
        endRemoveRows();
    } else if (!wasShowing && willShow) {
        beginInsertRows({}, realRows, realRows);
        footerState_ = s;
        endInsertRows();
    } else {
        // Both showing — same row, just text changes. Update via
        // dataChanged so the delegate repaints.
        footerState_ = s;
        const auto idx = index(realRows, 0);
        emit dataChanged(idx, idx,
            {PlaceholderTextRole, Qt::DisplayRole});
    }
}

bool MessageListModel::canFetchMore(const QModelIndex& parent) const {
    if (parent.isValid()) return false;
    // Search has no offset support in our FTS API (top-K only). Sticking
    // to false for search keeps Qt from poking fetchMore in a loop the
    // model can't actually fulfill. Both per-account ByLabel and
    // cross-account CrossAccountLabel sources DO paginate; the
    // underlying repository variants take limit/offset and walk
    // newest-first.
    if (source_ != Source::ByLabel
        && source_ != Source::CrossAccountLabel
        && source_ != Source::AllMail) return false;
    // The cache is drained for this source — wait for messagesUpdated /
    // a server top-up to bring back new rows. (loadFirstPage / explicit
    // refresh resets this flag.)
    return !cacheDrained_;
}

void MessageListModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid()) return;
    if (source_ != Source::ByLabel
        && source_ != Source::CrossAccountLabel
        && source_ != Source::AllMail) return;
    if (cacheDrained_) return;

    const int offset = static_cast<int>(rows_.size());
    std::vector<Message> more;
    if (source_ == Source::CrossAccountLabel) {
        more = conversationView_
            ? cache::MessageRepository::listThreadsByLabelAllAccounts(sourceParam_, pageSize(), offset)
            : cache::MessageRepository::listByLabelAllAccounts(sourceParam_, pageSize(), offset);
    } else if (source_ == Source::AllMail) {
        more = conversationView_
            ? cache::MessageRepository::listThreadsAllMail(accountId_, pageSize(), offset, unreadOnly_)
            : cache::MessageRepository::listAllMail(accountId_, pageSize(), offset, unreadOnly_);
    } else {
        more = conversationView_
            ? cache::MessageRepository::listThreadsByLabel(accountId_, sourceParam_, pageSize(), offset, unreadOnly_)
            : cache::MessageRepository::listByLabel(accountId_, sourceParam_, pageSize(), offset, unreadOnly_);
    }

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
    // page came back short. (For cross-account the owner's
    // cacheExhausted handler is a no-op since INBOX — the only
    // cross-account label today — is a seed kept fresh by every
    // account's incremental sync.)
    if (static_cast<int>(more.size()) < pageSize()) {
        cacheDrained_ = true;
        emit cacheExhausted(sourceParam_);
    }
}

void MessageListModel::expandLoadedRows(int targetRowCount) {
    if (source_ == Source::None) return;
    if (source_ == Source::BySearch) return;   // FTS has no offset
    if (targetRowCount <= 0) return;
    int parentRows = 0;
    for (const auto& m : rows_) if (!m.isThreadChild) ++parentRows;
    if (parentRows >= targetRowCount) return;   // already large enough

    const int limit = qMax(targetRowCount, pageSize());
    const int probe = limit + 1;

    std::vector<Message> rows;
    bool moreInCache = false;
    if (source_ == Source::ByLabel) {
        rows = conversationView_
            ? cache::MessageRepository::listThreadsByLabel(
                  accountId_, sourceParam_, probe, 0, unreadOnly_)
            : cache::MessageRepository::listByLabel(
                  accountId_, sourceParam_, probe, 0, unreadOnly_);
    } else if (source_ == Source::CrossAccountLabel) {
        rows = conversationView_
            ? cache::MessageRepository::listThreadsByLabelAllAccounts(
                  sourceParam_, probe, 0)
            : cache::MessageRepository::listByLabelAllAccounts(
                  sourceParam_, probe, 0);
    } else if (source_ == Source::AllMail) {
        rows = conversationView_
            ? cache::MessageRepository::listThreadsAllMail(
                  accountId_, probe, 0, unreadOnly_)
            : cache::MessageRepository::listAllMail(
                  accountId_, probe, 0, unreadOnly_);
    } else {
        return;
    }
    moreInCache = static_cast<int>(rows.size()) > limit;
    if (moreInCache) rows.pop_back();

    beginResetModel();
    rows_ = std::move(rows);
    expandedThreads_.clear();
    cacheDrained_ = !moreInCache;
    endResetModel();
}

void MessageListModel::resumeAfterTopUp() {
    // Top-up just finished — there should be more rows in the cache
    // below what we already have loaded. Clear the drain flag and
    // drain the cache into the model in 100-row chunks until we hit
    // either a short page (cache exhausted again — top-up will fire
    // for the next page if user scrolls) or a sane cap, so a single
    // server round-trip's worth of fresh rows lands in the view
    // without forcing the user to scroll-poke for each chunk.
    //
    // Cross-account doesn't go through topUpLabel today (every
    // contributing account's incremental sync keeps INBOX fresh on
    // its own). All Mail does — its top-up walks Gmail's full mailbox
    // page-by-page so resume is symmetric with ByLabel.
    if (source_ != Source::ByLabel && source_ != Source::AllMail) return;
    // Re-entry guard. fetchMore inside the loop emits cacheExhausted
    // on short pages, which the owner connects to topUpLabel ->
    // topUpFinished -> back here. Without this guard a tight cascade
    // of fetchMore -> cacheExhausted -> topUpLabel -> fetchMore can
    // recurse and confuse the rows_ index counters.
    if (resumingAfterTopUp_) return;
    QScopedValueRollback<bool> guard(resumingAfterTopUp_, true);
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

void MessageListModel::setCrossAccountLabelSource(const QString& labelId,
                                                    bool conversationView) {
    source_           = Source::CrossAccountLabel;
    sourceParam_      = labelId;
    conversationView_ = conversationView;
    // Unread-only filtering isn't wired through the *AllAccounts
    // repository variants yet; cross-account view always shows
    // everything for the label. Re-evaluate when v2 grows beyond
    // INBOX as the only cross-account label.
    unreadOnly_       = false;
    loadFirstPage();
}

void MessageListModel::refreshFromSource() {
    if (source_ == Source::None) return;
    // Re-query offset=0 with the current loaded count so a server top-up
    // / incremental sync surfaces without losing the user's scroll
    // window. The loadedRows fallback to pageSize() for the case where
    // we just initialized and rowCount is zero.
    //
    // Count only PARENT rows. expandedThreads_ preservation injects
    // child rows into rows_ — the cache queries
    // (listByLabel / listThreadsByLabel) return parents only, so a
    // limit derived from rows_.size() that includes children would
    // overshoot the actual cache content and the probe-based
    // moreInCache decision would set cacheDrained_ true prematurely.
    int parentRows = 0;
    for (const auto& m : rows_) if (!m.isThreadChild) ++parentRows;
    const int limit = qMax(parentRows, pageSize());

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
            ? cache::MessageRepository::listThreadsByLabel(accountId_, sourceParam_, probe, 0, unreadOnly_)
            : cache::MessageRepository::listByLabel(accountId_, sourceParam_, probe, 0, unreadOnly_);
        moreInCache = static_cast<int>(rows.size()) > limit;
        if (moreInCache) rows.pop_back();   // we only show `limit` rows
    } else if (source_ == Source::CrossAccountLabel) {
        rows = conversationView_
            ? cache::MessageRepository::listThreadsByLabelAllAccounts(sourceParam_, probe, 0)
            : cache::MessageRepository::listByLabelAllAccounts(sourceParam_, probe, 0);
        moreInCache = static_cast<int>(rows.size()) > limit;
        if (moreInCache) rows.pop_back();
    } else if (source_ == Source::AllMail) {
        rows = conversationView_
            ? cache::MessageRepository::listThreadsAllMail(accountId_, probe, 0, unreadOnly_)
            : cache::MessageRepository::listAllMail(accountId_, probe, 0, unreadOnly_);
        moreInCache = static_cast<int>(rows.size()) > limit;
        if (moreInCache) rows.pop_back();
    } else {
        // Search has no offset support, so the probe doesn't apply —
        // FTS top-K returns however many results match, capped at
        // `limit`.
        rows = conversationView_
            ? cache::MessageRepository::searchFtsThreads(accountId_, sourceParam_, limit)
            : cache::MessageRepository::searchFts(accountId_, sourceParam_, limit);
    }

    // Capture the user's currently-expanded thread set before the
    // reset clears it. After endResetModel we walk the new rows and
    // re-expand any thread that survived the refresh — without this
    // step, every messagesUpdated signal silently collapsed open
    // threads, which was jarring during incremental sync.
    const QSet<QString> savedExpansions = expandedThreads_;

    beginResetModel();
    rows_ = std::move(rows);
    expandedThreads_.clear();
    if (source_ == Source::BySearch) {
        cacheDrained_ = true;
    } else {
        // ByLabel, CrossAccountLabel and AllMail all rely on the probe.
        cacheDrained_ = !moreInCache;
    }
    endResetModel();

    if (savedExpansions.isEmpty()) return;
    // Re-expand each previously-open thread. Walk in REVERSE so
    // injecting children at row N doesn't shift the indices of rows
    // < N that we still want to inspect. Only act on parent rows
    // (isThreadChild==false) with multi-message threads — single-
    // message threads have nothing to inject and toggleThreadExpand
    // would be a no-op anyway.
    for (int i = static_cast<int>(rows_.size()) - 1; i >= 0; --i) {
        if (rows_[i].isThreadChild) continue;
        if (rows_[i].threadCount <= 1) continue;
        if (!savedExpansions.contains(rows_[i].threadId)) continue;
        toggleThreadExpand(i);
    }
}

void MessageListModel::loadFirstPage() {
    std::vector<Message> rows;
    if (source_ == Source::ByLabel) {
        rows = conversationView_
            ? cache::MessageRepository::listThreadsByLabel(accountId_, sourceParam_, pageSize(), 0, unreadOnly_)
            : cache::MessageRepository::listByLabel(accountId_, sourceParam_, pageSize(), 0, unreadOnly_);
    } else if (source_ == Source::CrossAccountLabel) {
        rows = conversationView_
            ? cache::MessageRepository::listThreadsByLabelAllAccounts(sourceParam_, pageSize(), 0)
            : cache::MessageRepository::listByLabelAllAccounts(sourceParam_, pageSize(), 0);
    } else if (source_ == Source::AllMail) {
        rows = conversationView_
            ? cache::MessageRepository::listThreadsAllMail(accountId_, pageSize(), 0, unreadOnly_)
            : cache::MessageRepository::listAllMail(accountId_, pageSize(), 0, unreadOnly_);
    } else if (source_ == Source::BySearch) {
        rows = conversationView_
            ? cache::MessageRepository::searchFtsThreads(accountId_, sourceParam_, pageSize())
            : cache::MessageRepository::searchFts(accountId_, sourceParam_, pageSize());
    }

    beginResetModel();
    rows_ = std::move(rows);
    expandedThreads_.clear();
    cacheDrained_ = (source_ == Source::BySearch)
        || static_cast<int>(rows_.size()) < pageSize();
    endResetModel();
}

void MessageListModel::setAccountId(const QString& accountId) {
    if (accountId_ == accountId) return;
    accountId_ = accountId;
    // Active per-account source needs to refetch under the new
    // accountId; CrossAccountLabel ignores accountId and replaceAll
    // sources are caller-driven, so we leave them alone.
    if (source_ == Source::ByLabel || source_ == Source::BySearch
        || source_ == Source::AllMail) {
        loadFirstPage();
    }
}

QString MessageListModel::sourceLabelId() const {
    if (source_ == Source::ByLabel
        || source_ == Source::CrossAccountLabel) return sourceParam_;
    if (source_ == Source::AllMail) return QStringLiteral("__all_mail");
    return {};
}

void MessageListModel::setAllMailSource(bool conversationView,
                                          bool unreadOnly) {
    source_           = Source::AllMail;
    sourceParam_      = QStringLiteral("__all_mail");
    conversationView_ = conversationView;
    unreadOnly_       = unreadOnly;
    loadFirstPage();
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
    //
    // We scope the lookup to the parent's accountId so a multi-account
    // unified inbox (v2) keeps thread expansion correct.
    // The parent row was loaded from a per-account or cross-account
    // source; either way it carries its source accountId. Fall back
    // to the model's pinned accountId for older rows that pre-date
    // AccountIdRole population.
    const QString lookupAccount = parent.accountId.isEmpty()
        ? accountId_ : parent.accountId;
    auto thread = fc::cache::MessageRepository::byThread(lookupAccount,
                                                         parent.threadId);
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
