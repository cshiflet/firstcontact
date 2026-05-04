#include "Theme.h"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QPalette>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QStyleFactory>

namespace fc::ui {

namespace {

constexpr char kSettingsKey[] = "ui/theme";

bool platformPrefersDark() {
    // Heuristic: compare the default palette's window background lightness.
    // Anything dimmer than mid-gray = "dark".
    const QPalette p;
    return p.color(QPalette::Window).lightness() < 128;
}

QString pickFontFamily() {
    // Prefer modern UI fonts in this order; fall back to whatever the system
    // reports as its default sans-serif. We don't bundle fonts (license
    // landmines, big binaries) — we just opt into a nicer one when present.
    static const QStringList candidates = {
        QStringLiteral("Inter"),
        QStringLiteral("SF Pro Text"),
        QStringLiteral("Segoe UI Variable"),
        QStringLiteral("Segoe UI"),
        QStringLiteral("Ubuntu"),
        QStringLiteral("Cantarell"),
        QStringLiteral("Noto Sans"),
        QStringLiteral("DejaVu Sans"),
    };
    const QStringList families = QFontDatabase::families();
    for (const auto& c : candidates) {
        if (families.contains(c)) return c;
    }
    return QApplication::font().family();
}

}  // namespace

QString Theme::modeToString(Mode m) {
    switch (m) {
        case Mode::Auto:  return QStringLiteral("auto");
        case Mode::Light: return QStringLiteral("light");
        case Mode::Dark:  return QStringLiteral("dark");
    }
    return QStringLiteral("auto");
}

Theme::Mode Theme::modeFromString(const QString& s) {
    if (s == QLatin1String("light")) return Mode::Light;
    if (s == QLatin1String("dark"))  return Mode::Dark;
    return Mode::Auto;
}

Theme::Mode Theme::resolveMode(Mode requested) {
    if (requested != Mode::Auto) return requested;
    return platformPrefersDark() ? Mode::Dark : Mode::Light;
}

Theme::Mode Theme::loadPersisted() {
    QSettings s;
    return modeFromString(s.value(QLatin1String(kSettingsKey),
                                  QStringLiteral("auto")).toString());
}

void Theme::persist(Mode m) {
    QSettings s;
    s.setValue(QLatin1String(kSettingsKey), modeToString(m));
}

void Theme::loadStylesheet(Mode resolved) {
    const QString path = (resolved == Mode::Dark)
        ? QStringLiteral(":/themes/dark.qss")
        : QStringLiteral(":/themes/light.qss");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning("Theme: failed to open %s", qUtf8Printable(path));
        return;
    }
    qApp->setStyleSheet(QString::fromUtf8(f.readAll()));
}

void Theme::apply(Mode mode) {
    persist(mode);

    // Fusion gives a consistent base across platforms (no native bevels) so
    // our QSS layers cleanly on top regardless of the host desktop's style.
    if (QStyleFactory::keys().contains(QStringLiteral("Fusion"),
                                       Qt::CaseInsensitive)) {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    }

    QFont f = QApplication::font();
    f.setFamily(pickFontFamily());
    if (f.pointSize() < 10) f.setPointSize(10);
    QApplication::setFont(f);

    const Mode resolved = resolveMode(mode);

    // Theme the standard palette roles QSS doesn't reach. QPalette::Link is
    // what QTextBrowser / QLabel use for <a href> anchor color when no
    // inline `color:` style is set; Fusion's default is a dark blue that's
    // unreadable against our dark surfaces.
    QPalette pal = QApplication::palette();
    if (resolved == Mode::Dark) {
        pal.setColor(QPalette::Link,        QColor(QStringLiteral("#8ab4f8")));
        pal.setColor(QPalette::LinkVisited, QColor(QStringLiteral("#c58af9")));
    } else {
        pal.setColor(QPalette::Link,        QColor(QStringLiteral("#1a73e8")));
        pal.setColor(QPalette::LinkVisited, QColor(QStringLiteral("#7c4dff")));
    }
    QApplication::setPalette(pal);

    loadStylesheet(resolved);

    emit instance()->changed(resolved);
}

void Theme::applyPersisted() {
    apply(loadPersisted());
}

Theme::Mode Theme::currentMode() {
    return loadPersisted();
}

Theme* Theme::instance() {
    static Theme inst;
    return &inst;
}

}  // namespace fc::ui
