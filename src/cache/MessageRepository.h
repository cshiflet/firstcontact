#pragma once

#include "models/Message.h"

#include <QString>
#include <QStringList>

#include <vector>

namespace fc::cache {

// CRUD over the `messages`, `message_labels`, and `attachments` tables.
// All methods open the per-thread sqlite handle internally; safe to call
// from the sync thread or the UI thread (each opens its own connection).
//
// Multi-account: every query is scoped to a single accountId, passed
// as the leading parameter. Cross-account "all accounts" variants are
// surfaced as separate methods (suffix `AllAccounts`).
//
// `unreadOnly` (where present) filters the listing to messages
// currently carrying the UNREAD label (or in conversation view,
// threads with at least one unread message). Mirrors Gmail web's
// filter chip.
class MessageRepository {
public:
    // ---------- Per-account API ----------

    // m.accountId must be non-empty; the row lands under that account.
    static qint64 upsert(const QString& accountId, const fc::Message& m);

    // Batched form. Wraps every per-message upsert in a single
    // transaction so the cost of fsync'ing the WAL goes from
    // O(messages) to O(1). For a 400-message initial-sync seed
    // listing that's a UI-thread freeze of ~500ms → ~50ms. Returns
    // the number of rows actually stored (failed individual upserts
    // skipped, transaction still commits the rest).
    static int upsertMany(const QString& accountId,
                          const std::vector<fc::Message>& msgs);

    static std::vector<fc::Message> listByLabel(const QString& accountId,
                                                const QString& labelId,
                                                int limit, int offset,
                                                bool unreadOnly = false);
    static std::vector<fc::Message> listThreadsByLabel(const QString& accountId,
                                                       const QString& labelId,
                                                       int limit, int offset,
                                                       bool unreadOnly = false);
    static std::vector<fc::Message> searchFts(const QString& accountId,
                                              const QString& query, int limit);
    static std::vector<fc::Message> searchFtsThreads(const QString& accountId,
                                                     const QString& query,
                                                     int limit);

    // "All Mail" — every cached message for `accountId` except those
    // carrying SPAM or TRASH. Mirrors Gmail web's All Mail virtual
    // folder (everything in the mailbox that isn't junk or deleted).
    // Drafts are kept in (Gmail's All Mail does the same).
    static std::vector<fc::Message> listAllMail(const QString& accountId,
                                                int limit, int offset,
                                                bool unreadOnly = false);
    static std::vector<fc::Message> listThreadsAllMail(
        const QString& accountId, int limit, int offset,
        bool unreadOnly = false);

    // ---------- Cross-account API (v2 unified inbox) ----------

    // Returns messages across every account that carries the given
    // label id (e.g. "INBOX"). Ordered newest-first across accounts.
    // Used by the "All Inboxes" sidebar entry.
    static std::vector<fc::Message> listByLabelAllAccounts(
        const QString& labelId, int limit, int offset);

    // Conversation-view variant. Threads are partitioned by
    // (account_id, thread_id) so a coincidence of thread ids across
    // accounts (rare; possible for shared aliases) doesn't fold.
    static std::vector<fc::Message> listThreadsByLabelAllAccounts(
        const QString& labelId, int limit, int offset);

    // Cross-account FTS5 search. The MATCH expression covers every
    // account; results carry the source accountId so the caller can
    // route a click back to the right context.
    static std::vector<fc::Message> searchFtsAllAccounts(
        const QString& query, int limit);
    static std::vector<fc::Message> searchFtsThreadsAllAccounts(
        const QString& query, int limit);
    static fc::Message              byId(const QString& accountId,
                                         const QString& id);
    static bool                     exists(const QString& accountId,
                                            const QString& id);
    static std::vector<fc::Message> byThread(const QString& accountId,
                                             const QString& threadId);
    static void                     applyLabelDiff(const QString& accountId,
                                                    const QString& messageId,
                                                    const QStringList& added,
                                                    const QStringList& removed);
    static void                     markAccessed(const QString& accountId,
                                                  const QString& id);
    static void                     setSnoozeUntil(const QString& accountId,
                                                    const QString& id,
                                                    qint64 wakeAtMs);
    static QStringList              dueSnoozeWakeups(const QString& accountId);
};

}  // namespace fc::cache
