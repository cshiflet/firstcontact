#pragma once

#include <QString>

namespace fc::cache {

class MetaRepository {
public:
    static QString get(const QString& key);
    static void    set(const QString& key, const QString& value);

    static QString historyId();
    static void    setHistoryId(const QString& v);
};

}  // namespace fc::cache
