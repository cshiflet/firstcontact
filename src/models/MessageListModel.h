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
    //
    // setCrossAccountLabelSource pins to the cross-account ("All
    // Inboxes") view: per-account scoping is dropped, the
    // *AllAccounts repository variants drive both the first page
    // and every fetchMore. Each row carries its source accountId
    // through AccountIdRole so a click can route back to the right
    // context.
    //
    // unreadOnly applies to the label source: when true, only messages
    // currently carrying UNREAD (or threads with at least one unread
    // message in conversation view) appear in the listing.
    void setLabelSource(const QString& labelId, bool conversationView,
                         bool unreadOnly = false);
    void setSearchSource(const QString& query, bool conversationView);
    void setCrossAccountLabelSource(const QString& labelId,
                                     bool conversationView);

    // Re-queries the active source for offset=0, limit=loadedRows — used by
    // messagesUpdated handlers to refresh in place without losing the
    // user's scroll window. Falls back to a no-op when the source has
    // never been set.
    void refreshFromSource();

    // Called when an out-of-band server top-up has filled the cache with
    // additional rows BELOW the currently-loaded window (typically older
    // messages). Resets the cacheDrained_ flag and invokes fetchMore
    // synchronously so the new rows append to the model — Qt's view
    // controllers won't ask for more rows once canFetchMore has returned
    // false, so we have to push.
    void resumeAfterTopUp();

    // Toggle inline expansion of the thread at `row`. No-op if the row
    // is already a child or represents a single-message thread. On
    // expand, the thread's older messages (latest-first) are loaded
    // from cache and inserted right after the parent row as children.
    // On collapse, those children are removed in place.
    void toggleThreadExpand(int row);

    QString sourceLabelId() const;

    // True when the last fetchMore returned 0 or a short page — i.e.,
    // the cache has nothing more to give for the current source.
    // Owners use this to drive a "No more messages" footer or to
    // decide whether to fire a server-side top-up.
    bool cacheDrained() const { return cacheDrained_; }

signals:
    // Fired when fetchMore was asked but the cache had no more rows for
    // the source. The owner connects this to SyncService::topUpLabel
    // (or any equivalent server-side fetcher) to bring back more rows.
    void cacheExhausted(const QString& labelId);

private:
    enum class Source { None, ByLabel, BySearch, CrossAccountLabel };

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
    bool    unreadOnly_       = false;
    bool    cacheDrained_     = false;   // last fetchMore returned 0
    // Re-entry guard for resumeAfterTopUp — set while we're inside
    // its drain loop. Without it, fetchMore can emit cacheExhausted
    // mid-loop which the owner reroutes back into topUpLabel /
    // topUpFinished and re-enters resumeAfterTopUp before the prior
    // call returns.
    bool    resumingAfterTopUp_ = false;
};

}  // namespace fc
