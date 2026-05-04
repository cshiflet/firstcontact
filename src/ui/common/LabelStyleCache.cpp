#include "LabelStyleCache.h"

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

void LabelStyleCache::invalidate() {
    cache_.clear();
    for (const auto& l : fc::cache::LabelRepository::all()) {
        Style s;
        s.name = l.name;
        s.type = l.type;
        if (!l.colorBg.isEmpty()) s.bg = QColor(l.colorBg);
        if (!l.colorFg.isEmpty()) s.fg = QColor(l.colorFg);
        cache_.insert(l.id, std::move(s));
    }
    emit changed();
}

LabelStyleCache::Style LabelStyleCache::get(const QString& labelId) const {
    return cache_.value(labelId);
}

}  // namespace fc::ui
