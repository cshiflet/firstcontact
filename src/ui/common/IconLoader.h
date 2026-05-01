#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

namespace fc::ui {

// Renders an SVG icon (stored as a Qt resource) tinted to a specific colour.
// Solves the "currentColor doesn't work in QIcon" problem — Qt's QIcon caches
// a rasterised pixmap from QSvgRenderer at construction time, so SVG
// `currentColor` references don't track the active QPalette / theme.
//
// We re-render the SVG into a pixmap with the requested stroke / fill colour
// substituted in, then wrap that in a QIcon. Cheap (icons are small) and
// gives us crisp colour-correct icons in both light and dark themes.
class IconLoader {
public:
    // Loads :/icons/symbolic/<name>.svg and tints all stroke/fill `currentColor`
    // (or the literal hex sentinel "#777777") references to the given colour.
    // Returns an empty QIcon on failure.
    static QIcon tinted(const QString& resourceName, const QColor& color);

    // Convenience: tint to the application's foreground text colour (so icons
    // automatically read against the active theme).
    static QIcon themed(const QString& resourceName);
};

}  // namespace fc::ui
