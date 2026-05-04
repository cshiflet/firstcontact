#include "Preferences.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

namespace fc::ui {

namespace {
constexpr char kKey[] = "html/preview";
constexpr char kAttachmentDirKey[] = "attachments/dir";
constexpr char kAttachmentAskKey[] = "attachments/alwaysAsk";
constexpr char kConversationKey[]  = "ui/conversationView";
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

QString Preferences::attachmentDir() {
    QSettings s;
    QString stored = s.value(QLatin1String(kAttachmentDirKey)).toString().trimmed();
    if (stored.isEmpty()) {
        stored = QStandardPaths::writableLocation(
                     QStandardPaths::DownloadLocation);
    }
    if (stored.isEmpty()) stored = QDir::homePath();
    return stored;
}

void Preferences::setAttachmentDir(const QString& dir) {
    QSettings s;
    s.setValue(QLatin1String(kAttachmentDirKey), dir);
    s.sync();
}

bool Preferences::alwaysAskAttachmentLocation() {
    QSettings s;
    return s.value(QLatin1String(kAttachmentAskKey), false).toBool();
}

void Preferences::setAlwaysAskAttachmentLocation(bool ask) {
    QSettings s;
    s.setValue(QLatin1String(kAttachmentAskKey), ask);
    s.sync();
}

bool Preferences::conversationView() {
    QSettings s;
    // Default ON to match Gmail web's default behaviour. Users who hate
    // it can flip it off in Settings.
    return s.value(QLatin1String(kConversationKey), true).toBool();
}

void Preferences::setConversationView(bool on) {
    QSettings s;
    s.setValue(QLatin1String(kConversationKey), on);
    s.sync();
}

}  // namespace fc::ui
