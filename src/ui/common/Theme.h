#pragma once

#include <QObject>
#include <QString>

class QApplication;

namespace fc::ui {

// Centralised look-and-feel control.
//
// Themes are shipped as QSS resources (:/themes/light.qss, :/themes/dark.qss)
// and applied to the running QApplication. The chosen theme is persisted to
// QSettings so the next launch starts in the same mode.
//
// "auto" mode follows the platform palette: dark when the desktop palette
// reports a dark window/text foreground at startup, light otherwise.
class Theme : public QObject {
    Q_OBJECT
public:
    enum class Mode { Auto, Light, Dark };

    // Apply the persisted theme on startup. Should be called from main
    // *before* the first widget is shown.
    static void applyPersisted();

    // Switch themes at runtime.
    static void apply(Mode mode);

    // The currently-active mode (resolved from auto if needed).
    static Mode currentMode();

    static QString modeToString(Mode m);
    static Mode    modeFromString(const QString& s);

    // The exact resolved theme that was applied (Light or Dark only).
    static Mode    resolveMode(Mode requested);

    // Singleton bus — widgets that need to repaint themselves on theme
    // changes (toolbar icons baked from the active palette, custom delegates,
    // anything that pre-renders against a theme colour) should connect to
    // changed().
    static Theme* instance();

signals:
    void changed(Mode resolved);

private:
    Theme() = default;
    static void loadStylesheet(Mode resolved);
    static void persist(Mode m);
    static Mode loadPersisted();
};

}  // namespace fc::ui
