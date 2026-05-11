#pragma once

#include <QString>

namespace fc::cache {

class MetaRepository {
public:
    // Global key/value sheet (schema_version, fts_version, etc.). The
    // single-arg form is reserved for genuinely global keys.
    static QString get(const QString& key);
    static void    set(const QString& key, const QString& value);

    // Per-account key/value sheet (account_meta table). v1 keys:
    //   - "history_id"          (sync delta cursor)
    //   - "email"                (canonical sender address)
    //   - "last_used_from"       (compose default; set on every send)
    //   - "notification_mode"    ("subject" or "envelope-only")
    //   - "color_hint"           (v3 accent palette key, v3+ only)
    static QString get(const QString& accountId, const QString& key);
    static void    set(const QString& accountId, const QString& key,
                       const QString& value);

    static QString historyId(const QString& accountId);
    static void    setHistoryId(const QString& accountId, const QString& v);
};

}  // namespace fc::cache
