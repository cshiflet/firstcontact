#pragma once

#include "models/Message.h"

#include <QString>
#include <QStringList>

#include <vector>

namespace fc::cache {

// CRUD over the `messages`, `message_labels`, and `attachments` tables.
// All methods open the per-thread sqlite handle internally; safe to call
// from the sync thread or the UI thread (each opens its own connection).
class MessageRepository {
public:
    // Upserts a message and replaces its label set. Returns the message rowid.
    static qint64 upsert(const fc::Message& m);

    // Loads the inbox-style listing for a label, ordered by internalDate desc.
    // Pulls metadata + snippet only — body is loaded on demand by `byId`.
    // When `unreadOnly` is true, the listing is filtered to messages
    // currently carrying the UNREAD label (mirrors Gmail web's filter
    // chip).
    static std::vector<fc::Message> listByLabel(const QString& labelId,
                                                int limit, int offset,
                                                bool unreadOnly = false);

    // Conversation-view variant: groups messages by thread_id and returns
    // one row per thread, hydrated with the LATEST message's headers plus
    // whole-thread aggregates (threadCount, threadHasUnread,
    // threadHasStarred, threadHasAttachment). The row's `id` is the latest
    // message's id so opening + marking-read still work the same way.
    // Used when Preferences::conversationView() is true.
    // unreadOnly filters to threads with at least one unread message.
    static std::vector<fc::Message> listThreadsByLabel(const QString& labelId,
                                                       int limit, int offset,
                                                       bool unreadOnly = false);

    // Full-text search via FTS5; results ordered by rank then internalDate.
    // The query is normalised internally to a safe FTS5 expression so callers
    // can pass arbitrary user input without risking syntax errors.
    static std::vector<fc::Message> searchFts(const QString& query, int limit);

    // Conversation-view variant of searchFts: same FTS5 hits, then collapsed
    // to one row per thread via window functions (latest message wins).
    static std::vector<fc::Message> searchFtsThreads(const QString& query,
                                                      int limit);

    static fc::Message byId(const QString& id);
    static bool exists(const QString& id);

    // Returns all messages in a thread, ordered ascending by internal_date.
    static std::vector<fc::Message> byThread(const QString& threadId);

    // Apply local label add/remove (and update is_unread/starred derived flags).
    static void applyLabelDiff(const QString& messageId,
                               const QStringList& added,
                               const QStringList& removed);

    // Touches `last_accessed_at` for the LRU evictor.
    static void markAccessed(const QString& id);

    // Snooze: sets / clears the per-message snooze_until ms-epoch.
    // 0 / negative clears. Caller is responsible for the matching
    // INBOX-label transition (drop INBOX on snooze, restore on wake)
    // — the column is purely the "wake at time T" hint.
    static void setSnoozeUntil(const QString& id, qint64 wakeAtMs);

    // Returns ids of messages whose snooze window has lapsed
    // (snooze_until IS NOT NULL AND snooze_until <= now). Used by the
    // wake-up tick in MainWindow; the caller restores INBOX and
    // clears snooze_until per id.
    static QStringList dueSnoozeWakeups();
};

}  // namespace fc::cache
