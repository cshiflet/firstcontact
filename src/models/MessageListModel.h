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

    // Pagination hooks. Qt's view controllers call canFetchMore + fetchMore
    // automatically as the user scrolls past the bottom of what's loaded.
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    // Kept around for any caller that wants to slam an explicit list into
    // the model (search results, ad-hoc fixture loaders). For browsing
    // labels, prefer setLabelSource so canFetchMore can keep paging.
    void replaceAll(std::vector<Message> messages);
    const Message* messageAt(int row) const;

    // Source-pinned modes. setLabelSource resets the model and loads the
    // first cache page for the label; subsequent fetchMore calls walk
    // the cache forward. setSearchSource pins to FTS5 search results
    // (no pagination — the FTS API is top-K only).
    void setLabelSource(const QString& labelId, bool conversationView);
    void setSearchSource(const QString& query, bool conversationView);

    // Re-queries the active source for offset=0, limit=loadedRows — used by
    // messagesUpdated handlers to refresh in place without losing the
    // user's scroll window. Falls back to a no-op when the source has
    // never been set.
    void refreshFromSource();

    // Toggle inline expansion of the thread at `row`. No-op if the row
    // is already a child or represents a single-message thread. On
    // expand, the thread's older messages (latest-first) are loaded
    // from cache and inserted right after the parent row as children.
    // On collapse, those children are removed in place.
    void toggleThreadExpand(int row);

    QString sourceLabelId() const;

signals:
    // Fired when fetchMore was asked but the cache had no more rows for
    // the source. The owner connects this to SyncService::topUpLabel
    // (or any equivalent server-side fetcher) to bring back more rows.
    void cacheExhausted(const QString& labelId);

private:
    enum class Source { None, ByLabel, BySearch };

    int  pageSize() const { return 100; }
    void loadFirstPage();

    std::vector<Message> rows_;
    // Thread ids whose children are currently shown inline. Cleared on
    // every replaceAll — switching label / refresh resets expansion,
    // matching what most desktop mail clients do.
    QSet<QString> expandedThreads_;

    Source  source_           = Source::None;
    QString sourceParam_;     // labelId or search query
    bool    conversationView_ = true;
    bool    cacheDrained_     = false;   // last fetchMore returned 0
};

}  // namespace fc
