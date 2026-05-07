#pragma once

#include <QString>

#include <vector>

namespace fc::cache {

struct LabelRow {
    QString accountId;
    QString id;
    QString name;
    QString type;          // "system" | "user"
    QString parentId;
    int     unreadCount = 0;
    int     totalCount  = 0;
    QString colorBg;
    QString colorFg;
};

class LabelRepository {
public:
    // Per-account API — preferred. Step-3 of the multi-account migration
    // moves every call site to these.
    static void                  upsert(const QString& accountId,
                                        const LabelRow& l);
    static std::vector<LabelRow> all(const QString& accountId);
    static LabelRow              byId(const QString& accountId,
                                      const QString& id);
    static void                  remove(const QString& accountId,
                                        const QString& id);
    static void                  recomputeCounts(const QString& accountId);

    // TODO(v2): cross-account label aggregation for the unified inbox /
    // search overlay. Until then, callers stick to the per-account form.

    // Legacy single-account overloads. Forward to the default account
    // (Database::defaultAccountId). Once step 3 finishes, only a few
    // entry points still need these and they go away in step 12.
    static void                  upsert(const LabelRow& l);
    static std::vector<LabelRow> all();
    static LabelRow              byId(const QString& id);
    static void                  remove(const QString& id);
    static void                  recomputeCounts();
};

}  // namespace fc::cache
