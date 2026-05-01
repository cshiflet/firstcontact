#pragma once

#include <QString>

#include <vector>

namespace fc::cache {

struct LabelRow {
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
    static void                  upsert(const LabelRow& l);
    static std::vector<LabelRow> all();
    static LabelRow              byId(const QString& id);
    static void                  remove(const QString& id);

    // Recompute unread/total counts from message_labels — call once after
    // initial sync and after batch deltas.
    static void recomputeCounts();
};

}  // namespace fc::cache
