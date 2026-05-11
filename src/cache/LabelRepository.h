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
    static void                  upsert(const QString& accountId,
                                        const LabelRow& l);
    static std::vector<LabelRow> all(const QString& accountId);
    static LabelRow              byId(const QString& accountId,
                                      const QString& id);
    static void                  remove(const QString& accountId,
                                        const QString& id);
    static void                  recomputeCounts(const QString& accountId);

    // Cross-account variant. Returns every label across every account
    // — each row carries its source accountId so the caller can route
    // back. Used by the v2 unified search overlay so pills paint
    // correctly even when results span accounts.
    static std::vector<LabelRow> allAccounts();
};

}  // namespace fc::cache
