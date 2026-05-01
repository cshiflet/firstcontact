#include "Preferences.h"

#include <QSettings>

namespace fc::ui {

namespace {
constexpr char kKey[] = "html/preview";
}

const char* Preferences::htmlPreviewKey() { return kKey; }

QString Preferences::htmlPreviewToString(HtmlPreview m) {
    switch (m) {
        case HtmlPreview::Disabled:        return QStringLiteral("disabled");
        case HtmlPreview::ExternalBrowser: return QStringLiteral("external");
        case HtmlPreview::InlineWebEngine: return QStringLiteral("inline");
    }
    return QStringLiteral("external");
}

Preferences::HtmlPreview Preferences::htmlPreviewFromString(const QString& s) {
    if (s == QLatin1String("disabled")) return HtmlPreview::Disabled;
    if (s == QLatin1String("inline"))   return HtmlPreview::InlineWebEngine;
    return HtmlPreview::ExternalBrowser;   // safe default
}

Preferences::HtmlPreview Preferences::htmlPreview() {
    QSettings s;
    return htmlPreviewFromString(
        s.value(QLatin1String(kKey), QStringLiteral("external")).toString());
}

void Preferences::setHtmlPreview(HtmlPreview m) {
    QSettings s;
    s.setValue(QLatin1String(kKey), htmlPreviewToString(m));
    s.sync();
}

}  // namespace fc::ui
