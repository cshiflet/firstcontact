#include "Preferences.h"

#include "util/PageSizePref.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

namespace fc::ui {

namespace {
constexpr char kKey[] = "html/preview";
constexpr char kAttachmentDirKey[]    = "attachments/dir";
constexpr char kConversationKey[]     = "ui/conversationView";
constexpr char kToolbarShowTextKey[]  = "ui/toolbarShowText";
constexpr char kSidebarColorsKey[]    = "ui/sidebarLabelColors";
constexpr char kMessageListPillsKey[] = "ui/messageListLabelPills";
constexpr char kImageProxyKey[]       = "html/imageProxyUrlPattern";
constexpr char kStripPixelsKey[]      = "html/stripTrackingPixels";
constexpr char kDefaultImageProxy[]   = "https://wsrv.nl/?url={url}";
constexpr char kSignatureKey[]        = "compose/signature";
constexpr char kReplyAboveKey[]       = "compose/replyAboveOriginal";
constexpr char kQuoteStyleKey[]       = "compose/quoteStyle";
constexpr char kUnreadOnlyKey[]       = "filter/unreadOnly";
constexpr char kUiFontScaleKey[]      = "ui/fontScale";
constexpr char kLinkModeKey[]         = "ui/linkDisplayMode";
}

const char* Preferences::htmlPreviewKey() { return kKey; }

QString Preferences::htmlPreviewToString(HtmlPreview m) {
    switch (m) {
        case HtmlPreview::Disabled:        return QStringLiteral("disabled");
        case HtmlPreview::ExternalBrowser: return QStringLiteral("external");
    }
    return QStringLiteral("external");
}

Preferences::HtmlPreview Preferences::htmlPreviewFromString(const QString& s) {
    if (s == QLatin1String("disabled")) return HtmlPreview::Disabled;
    // Stored value of "inline" silently migrates to External Browser —
    // the inline WebEngine path was removed (see the "Drop inline
    // WebEngine HTML preview" commit). Users who had it selected get
    // the next-closest equivalent without a popup.
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

bool Preferences::toolbarShowText() {
    QSettings s;
    return s.value(QLatin1String(kToolbarShowTextKey), true).toBool();
}

void Preferences::setToolbarShowText(bool on) {
    QSettings s;
    s.setValue(QLatin1String(kToolbarShowTextKey), on);
    s.sync();
}

bool Preferences::sidebarLabelColors() {
    QSettings s;
    return s.value(QLatin1String(kSidebarColorsKey), true).toBool();
}

void Preferences::setSidebarLabelColors(bool on) {
    QSettings s;
    s.setValue(QLatin1String(kSidebarColorsKey), on);
    s.sync();
}

bool Preferences::messageListLabelPills() {
    QSettings s;
    return s.value(QLatin1String(kMessageListPillsKey), true).toBool();
}

void Preferences::setMessageListLabelPills(bool on) {
    QSettings s;
    s.setValue(QLatin1String(kMessageListPillsKey), on);
    s.sync();
}

QString Preferences::imageProxyUrlPattern() {
    QSettings s;
    const QString stored = s.value(QLatin1String(kImageProxyKey),
                                    QLatin1String(kDefaultImageProxy))
                              .toString();
    return stored;
}

void Preferences::setImageProxyUrlPattern(const QString& pattern) {
    QSettings s;
    if (pattern.trimmed().isEmpty()) {
        s.remove(QLatin1String(kImageProxyKey));
    } else {
        s.setValue(QLatin1String(kImageProxyKey), pattern.trimmed());
    }
    s.sync();
}

bool Preferences::stripTrackingPixels() {
    QSettings s;
    return s.value(QLatin1String(kStripPixelsKey), false).toBool();
}

void Preferences::setStripTrackingPixels(bool on) {
    QSettings s;
    s.setValue(QLatin1String(kStripPixelsKey), on);
    s.sync();
}

QString Preferences::signatureText() {
    QSettings s;
    return s.value(QLatin1String(kSignatureKey), QString()).toString();
}

void Preferences::setSignatureText(const QString& text) {
    QSettings s;
    if (text.isEmpty()) {
        s.remove(QLatin1String(kSignatureKey));
    } else {
        s.setValue(QLatin1String(kSignatureKey), text);
    }
    s.sync();
}

bool Preferences::replyAboveOriginal() {
    QSettings s;
    // Default to top-posting: matches Gmail web's reply UX, which is
    // what most users will expect coming from a Gmail-style client.
    return s.value(QLatin1String(kReplyAboveKey), true).toBool();
}

void Preferences::setReplyAboveOriginal(bool on) {
    QSettings s;
    s.setValue(QLatin1String(kReplyAboveKey), on);
    s.sync();
}

QString Preferences::quoteStyleToString(QuoteStyle q) {
    switch (q) {
        case QuoteStyle::BlockQuote:    return QStringLiteral("blockquote");
        case QuoteStyle::GreaterPrefix: return QStringLiteral("greater");
    }
    return QStringLiteral("blockquote");
}

Preferences::QuoteStyle Preferences::quoteStyleFromString(const QString& s) {
    if (s == QLatin1String("greater")) return QuoteStyle::GreaterPrefix;
    return QuoteStyle::BlockQuote;
}

Preferences::QuoteStyle Preferences::quoteStyle() {
    QSettings s;
    return quoteStyleFromString(
        s.value(QLatin1String(kQuoteStyleKey),
                QStringLiteral("blockquote")).toString());
}

void Preferences::setQuoteStyle(QuoteStyle q) {
    QSettings s;
    s.setValue(QLatin1String(kQuoteStyleKey), quoteStyleToString(q));
    s.sync();
}

bool Preferences::unreadOnly() {
    QSettings s;
    return s.value(QLatin1String(kUnreadOnlyKey), false).toBool();
}

void Preferences::setUnreadOnly(bool on) {
    QSettings s;
    s.setValue(QLatin1String(kUnreadOnlyKey), on);
    s.sync();
}

double Preferences::uiFontScale() {
    QSettings s;
    const double v = s.value(QLatin1String(kUiFontScaleKey), 1.0).toDouble();
    return qBound(0.75, v, 3.0);
}

void Preferences::setUiFontScale(double scale) {
    QSettings s;
    s.setValue(QLatin1String(kUiFontScaleKey), qBound(0.75, scale, 3.0));
    s.sync();
}

fc::util::LinkDisplayMode Preferences::linkDisplayMode() {
    QSettings s;
    const QString v = s.value(QLatin1String(kLinkModeKey),
                               QStringLiteral("labeled")).toString();
    if (v == QLatin1String("full")) return fc::util::LinkDisplayMode::FullUrl;
    return fc::util::LinkDisplayMode::Labeled;
}

void Preferences::setLinkDisplayMode(fc::util::LinkDisplayMode m) {
    QSettings s;
    s.setValue(QLatin1String(kLinkModeKey),
                m == fc::util::LinkDisplayMode::FullUrl
                    ? QStringLiteral("full")
                    : QStringLiteral("labeled"));
    s.sync();
}

int  Preferences::messagePageSize()      { return fc::util::messagePageSize(); }
void Preferences::setMessagePageSize(int n) { fc::util::setMessagePageSize(n); }

namespace {
constexpr char kLowBandwidthKey[]            = "sync/lowBandwidthMode";
constexpr char kBackgroundCrawlKey[]         = "sync/backgroundCrawl";
constexpr char kBackgroundCrawlIntervalKey[] = "sync/backgroundCrawlIntervalSec";
}

bool Preferences::lowBandwidthMode() {
    QSettings s;
    return s.value(QLatin1String(kLowBandwidthKey), false).toBool();
}

void Preferences::setLowBandwidthMode(bool on) {
    QSettings s;
    s.setValue(QLatin1String(kLowBandwidthKey), on);
    s.sync();
}

bool Preferences::backgroundCrawl() {
    QSettings s;
    return s.value(QLatin1String(kBackgroundCrawlKey), false).toBool();
}

void Preferences::setBackgroundCrawl(bool on) {
    QSettings s;
    s.setValue(QLatin1String(kBackgroundCrawlKey), on);
    s.sync();
}

int Preferences::backgroundCrawlIntervalSec() {
    QSettings s;
    int v = s.value(QLatin1String(kBackgroundCrawlIntervalKey), 5).toInt();
    return qMax(1, v);
}

void Preferences::setBackgroundCrawlIntervalSec(int seconds) {
    QSettings s;
    s.setValue(QLatin1String(kBackgroundCrawlIntervalKey), qMax(1, seconds));
    s.sync();
}

namespace {
constexpr char kDbCompressionKey[]            = "compression/enabled";
constexpr char kDbCompressionPromptShownPfx[] = "compression/promptShown/";
}

bool Preferences::dbCompression() {
    QSettings s;
    return s.value(QLatin1String(kDbCompressionKey), true).toBool();
}

void Preferences::setDbCompression(bool on) {
    QSettings s;
    s.setValue(QLatin1String(kDbCompressionKey), on);
    s.sync();
}

bool Preferences::dbCompressionPromptShown(const QString& accountId) {
    if (accountId.isEmpty()) return false;
    QSettings s;
    return s.value(QLatin1String(kDbCompressionPromptShownPfx) + accountId,
                    false).toBool();
}

void Preferences::setDbCompressionPromptShown(const QString& accountId,
                                                bool shown) {
    if (accountId.isEmpty()) return;
    QSettings s;
    s.setValue(QLatin1String(kDbCompressionPromptShownPfx) + accountId, shown);
    s.sync();
}

namespace {
constexpr char kCacheAutoPruneKey[] = "cache/autoPrune";
constexpr char kCacheMaxAgeKey[]    = "cache/maxAgeDays/";   // + accountId
constexpr char kCacheMaxMsgsKey[]   = "cache/maxMessages/";  // + accountId
constexpr char kCacheMaxMbKey[]     = "cache/maxCacheMb/";   // + accountId
}

bool Preferences::cacheAutoPrune() {
    QSettings s;
    return s.value(QLatin1String(kCacheAutoPruneKey), false).toBool();
}

void Preferences::setCacheAutoPrune(bool on) {
    QSettings s;
    s.setValue(QLatin1String(kCacheAutoPruneKey), on);
    s.sync();
}

int Preferences::cacheMaxAgeDays(const QString& accountId) {
    if (accountId.isEmpty()) return 0;
    QSettings s;
    return s.value(QLatin1String(kCacheMaxAgeKey) + accountId, 0).toInt();
}

void Preferences::setCacheMaxAgeDays(const QString& accountId, int days) {
    if (accountId.isEmpty()) return;
    QSettings s;
    s.setValue(QLatin1String(kCacheMaxAgeKey) + accountId, qMax(0, days));
    s.sync();
}

int Preferences::cacheMaxMessages(const QString& accountId) {
    if (accountId.isEmpty()) return 0;
    QSettings s;
    return s.value(QLatin1String(kCacheMaxMsgsKey) + accountId, 0).toInt();
}

void Preferences::setCacheMaxMessages(const QString& accountId, int n) {
    if (accountId.isEmpty()) return;
    QSettings s;
    s.setValue(QLatin1String(kCacheMaxMsgsKey) + accountId, qMax(0, n));
    s.sync();
}

int Preferences::cacheMaxCacheMb(const QString& accountId) {
    if (accountId.isEmpty()) return 0;
    QSettings s;
    return s.value(QLatin1String(kCacheMaxMbKey) + accountId, 0).toInt();
}

void Preferences::setCacheMaxCacheMb(const QString& accountId, int mb) {
    if (accountId.isEmpty()) return;
    QSettings s;
    s.setValue(QLatin1String(kCacheMaxMbKey) + accountId, qMax(0, mb));
    s.sync();
}

}  // namespace fc::ui
