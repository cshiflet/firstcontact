#include "LabelStyleCache.h"

#include "cache/Database.h"
#include "cache/LabelRepository.h"

namespace fc::ui {

LabelStyleCache& LabelStyleCache::instance() {
    static LabelStyleCache cache;
    return cache;
}

LabelStyleCache::LabelStyleCache() {
    // Hot-path the initial load so views constructed before the first
    // sync still have something useful to paint.
    invalidate();
}

void LabelStyleCache::invalidate(const QString& accountId) {
    cache_.clear();
    if (!accountId.isEmpty()) {
        for (const auto& l : fc::cache::LabelRepository::all(accountId)) {
            Style s;
            s.name = l.name;
            s.type = l.type;
            if (!l.colorBg.isEmpty()) s.bg = QColor(l.colorBg);
            if (!l.colorFg.isEmpty()) s.fg = QColor(l.colorFg);
            cache_.insert(l.id, std::move(s));
        }
    }
    emit changed();
}

void LabelStyleCache::invalidate() {
    invalidate(fc::cache::Database::defaultAccountId());
}

LabelStyleCache::Style LabelStyleCache::get(const QString& labelId) const {
    return cache_.value(labelId);
}

}  // namespace fc::ui
