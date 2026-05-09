#include "ReaderPane.h"

#include "LocalHtmlServer.h"
#include "cache/MetaRepository.h"
#include "util/ImageProxy.h"
#include "ui/common/IconLoader.h"
#include "ui/common/Preferences.h"
#include "ui/common/Theme.h"
#include "util/Browser.h"
#include "util/HtmlSanitizer.h"
#include "util/Html2Text.h"
#include "util/Linkify.h"

#include <QAction>
#include <QDateTime>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <memory>

namespace fc::ui {

namespace {

QString formatDate(qint64 ms) {
    if (ms <= 0) return {};
    return QDateTime::fromMSecsSinceEpoch(ms).toLocalTime()
        .toString(QStringLiteral("ddd, MMM d, yyyy h:mm AP"));
}

QString fromDisplay(const fc::Message& m) {
    // Decode pre-encoded entities from the cached header before HTML-escaping
    // for rich-text rendering. Without this step a stale cached fromName
    // like "Bob &amp; Co" would round-trip through toHtmlEscaped as
    // "Bob &amp;amp; Co" and render literally.
    const QString name = util::decodeHtmlEntities(m.fromName);
    const QString addr = util::decodeHtmlEntities(m.fromAddr);
    if (!name.isEmpty() && !addr.isEmpty())
        return QStringLiteral("%1 &lt;%2&gt;")
            .arg(name.toHtmlEscaped(), addr.toHtmlEscaped());
    return addr.toHtmlEscaped();
}

// Active-theme color tokens. We mirror the values in resources/themes/*.qss
// here because QPalette doesn't reflect QSS-set colors and the inline styles
// inside our header HTML need to render against the current theme.
struct ThemeColors {
    QString primary;
    QString secondary;
    QString warningBg;
    QString warningBorder;
    QString warningText;
};

ThemeColors themeColors() {
    const bool dark = Theme::resolveMode(Theme::currentMode())
                       == Theme::Mode::Dark;
    if (dark) {
        return {
            QStringLiteral("#f1f3f4"),  // primary text — near-white for contrast
            QStringLiteral("#9aa0a6"),  // secondary text
            QStringLiteral("#3a341a"),  // warning bg (dim amber)
            QStringLiteral("#5e5021"),  // warning border
            QStringLiteral("#fdd663"),  // warning text
        };
    }
    return {
        // Push primary all the way to near-black so the rich-text inline
        // styles win against any palette-inherited grey, especially under
        // Fusion + QSS where palette colors don't always cascade into
        // QLabel's QTextDocument the way a naive reader would expect.
        QStringLiteral("#0d0d0d"),
        QStringLiteral("#5f6368"),
        QStringLiteral("#fff8c4"),
        QStringLiteral("#d4d000"),
        QStringLiteral("#5e5500"),
    };
}

QString headerHtml(const fc::Message& m, bool full) {
    const ThemeColors c = themeColors();
    const QString subject = util::decodeHtmlEntities(m.subject);
    // Scale the inline pt sizes by the user's UI scale preference —
    // these are inline HTML styles, NOT QSS rules, so the regex pass
    // in Theme::loadStylesheet doesn't reach them.
    const double scale = Preferences::uiFontScale();
    auto pt = [scale](int n) { return qMax(1, qRound(n * scale)); };
    QString h = QStringLiteral(
        "<div style='font-weight:600; font-size:%1pt; line-height:1.3; color:%2;'>%3</div>"
        "<div style='color:%4; font-size:%5pt; margin-top:2px;'>"
        "<span style='font-weight:500; color:%2;'>%6</span> &nbsp;·&nbsp; %7</div>")
        .arg(pt(13))
        .arg(c.primary)
        .arg(subject.isEmpty()
                 ? QStringLiteral("<i>(no subject)</i>")
                 : subject.toHtmlEscaped())
        .arg(c.secondary)
        .arg(pt(10))
        .arg(fromDisplay(m))
        .arg(formatDate(m.internalDate));
    if (full) {
        if (!m.toAddrs.isEmpty()) {
            h += QStringLiteral("<div style='color:%1; font-size:%2pt; margin-top:4px;'>"
                                "<b>to</b> %3</div>")
                    .arg(c.secondary)
                    .arg(pt(9))
                    .arg(m.toAddrs.join(QStringLiteral(", ")).toHtmlEscaped());
        }
        if (!m.ccAddrs.isEmpty()) {
            h += QStringLiteral("<div style='color:%1; font-size:%2pt;'>"
                                "<b>cc</b> %3</div>")
                    .arg(c.secondary)
                    .arg(pt(9))
                    .arg(m.ccAddrs.join(QStringLiteral(", ")).toHtmlEscaped());
        }
    }
    return h;
}

// QTextBrowser variant that height-for-widths to the rendered document
// height instead of the default 192-px sizeHint with no relationship
// to content. Without this every message card in a thread stacked at
// either the 360-px minimum (huge for one-line replies) or the
// QTextEdit default (still arbitrary). With heightForWidth, the
// outer QScrollArea handles vertical scrolling; the body itself
// reports exactly the height its document needs.
class AutoSizeTextBrowser : public QTextBrowser {
public:
    using QTextBrowser::QTextBrowser;

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int w) const override {
        QTextDocument* doc = document();
        const qreal saved = doc->textWidth();
        // Account for the QTextEdit frame + viewport horizontal margins
        // so the document doesn't reflow when later painted into the
        // viewport.
        const int textW = qMax(50, w - 4);
        doc->setTextWidth(textW);
        const int h = static_cast<int>(doc->size().height())
                    + static_cast<int>(2 * doc->documentMargin())
                    + 4;
        doc->setTextWidth(saved);
        return h;
    }

    QSize sizeHint() const override {
        const int w = viewport() && viewport()->width() > 50
            ? viewport()->width() : 600;
        return { w, heightForWidth(w) };
    }
    QSize minimumSizeHint() const override { return { 0, 40 }; }

protected:
    void resizeEvent(QResizeEvent* e) override {
        QTextBrowser::resizeEvent(e);
        // updateGeometry() so the parent layout re-asks for our new
        // heightForWidth after a width change.
        updateGeometry();
    }
};

QString humanSize(qint64 bytes) {
    if (bytes <= 0) return {};
    constexpr double KB = 1024.0;
    constexpr double MB = KB * 1024.0;
    constexpr double GB = MB * 1024.0;
    if (bytes < KB)        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < MB)        return QStringLiteral("%1 KB").arg(bytes / KB, 0, 'f', 1);
    if (bytes < GB)        return QStringLiteral("%1 MB").arg(bytes / MB, 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(bytes / GB, 0, 'f', 2);
}

// Some senders (notably marketers using ESPs that auto-generate text/plain
// from text/html via naive tag-strip) produce a "plain text" part that is
// one giant paragraph: thousands of characters, no newlines. Showing that
// in the reader is worse than re-deriving plain text from the HTML body
// (which we control via Html2Text and preserve paragraph breaks for).
//
// Heuristic: if any line in bodyText is longer than this and there's a
// non-empty bodyHtml available, prefer html2text(bodyHtml). 600 chars is
// well above any real prose paragraph; only auto-generated walls of text
// trip it.
constexpr int kLossyPlainTextLineThreshold = 600;

bool plainTextLooksLossy(const QString& text) {
    if (text.size() < kLossyPlainTextLineThreshold) return false;
    int run = 0;
    for (QChar c : text) {
        if (c == QChar('\n')) { run = 0; continue; }
        if (++run > kLossyPlainTextLineThreshold) return true;
    }
    return false;
}

QString bodyHtml(const fc::Message& m) {
    // Prefer the HTML body when the plain-text part looks auto-generated
    // and lossy — we can re-derive plain text from HTML with paragraph
    // structure intact via util::html2text. The original bodyText path
    // still wins when text/plain is genuinely useful (most personal
    // mail, mailing-list digests, and any sender who composed a
    // text-first body).
    const bool preferHtmlOverLossyText = !m.bodyHtml.isEmpty()
        && plainTextLooksLossy(m.bodyText);

    if (!m.bodyText.isEmpty() && !preferHtmlOverLossyText) {
        return util::linkifyPlainText(m.bodyText, Preferences::linkDisplayMode());
    }
    if (!m.bodyHtml.isEmpty()) {
        if (preferHtmlOverLossyText) {
            // Tier 1 of the "I'd rather see structured plain text" mode:
            // tag-strip the HTML to a paragraph-preserving plain string,
            // then linkify. Sanitisation isn't required here — html2text
            // drops every tag — but we still feed the result through
            // linkifyPlainText so URLs render as clickable anchors.
            const QString rebuilt = util::html2text(m.bodyHtml);
            return util::linkifyPlainText(rebuilt, Preferences::linkDisplayMode());
        }
        const auto safe = util::sanitizeHtml(m.bodyHtml);
        QString r = safe.html;
        if (safe.remoteImagesBlocked) {
            const ThemeColors c = themeColors();
            r.prepend(QStringLiteral(
                "<div style='background:%1;padding:6px;border:1px solid %2;"
                "color:%3;'><i>Remote images blocked.</i></div>")
                .arg(c.warningBg, c.warningBorder, c.warningText));
        }
        return r;
    }
    if (m.bodyHtmlPresent) {
        return QStringLiteral("<p><i>HTML content not yet fetched.</i></p>");
    }
    return QStringLiteral("<p><i>(empty message)</i></p>");
}

}  // namespace

ReaderPane::ReaderPane(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("ReaderPane"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    scroll_ = new QScrollArea(this);
    scroll_->setObjectName(QStringLiteral("ReaderScroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);

    content_ = new QWidget(scroll_);
    content_->setObjectName(QStringLiteral("ReaderContent"));
    contentLayout_ = new QVBoxLayout(content_);
    contentLayout_->setContentsMargins(14, 12, 14, 14);
    contentLayout_->setSpacing(14);
    contentLayout_->addStretch(1);

    scroll_->setWidget(content_);
    root->addWidget(scroll_);

    showEmpty();
}

void ReaderPane::clearStack() {
    while (auto* item = contentLayout_->takeAt(0)) {
        if (auto* w = item->widget()) w->deleteLater();
        delete item;
    }
    contentLayout_->addStretch(1);
}

QWidget* ReaderPane::buildMessageCard(const fc::Message& m, bool initiallyExpanded) {
    auto* card = new QFrame(content_);
    card->setObjectName(QStringLiteral("MessageCard"));
    // Tag the card with its message id so showThread's focus-and-scroll
    // pass can find it after layout settles. Plain QString property —
    // findChild walks by objectName, but we filter on this property
    // because objectName is shared across all cards.
    card->setProperty("messageId", m.id);
    card->setFrameShape(QFrame::NoFrame);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(12, 10, 12, 10);
    cardLayout->setSpacing(10);

    // Soft drop-shadow under the card. Cheaper than QSS box-shadow (which Qt
    // doesn't support) and gives the modern Material/Gmail floating-card feel.
    auto* shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(18);
    shadow->setOffset(0, 2);
    shadow->setColor(QColor(0, 0, 0, 28));
    card->setGraphicsEffect(shadow);

    auto* header = new QLabel(card);
    header->setTextFormat(Qt::RichText);
    header->setWordWrap(true);
    header->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Belt-and-braces: also set the QLabel's foreground via QPalette in
    // case the QLabel's QTextDocument decides to override our inline style
    // colors with the inherited palette text. Both the inline color: rules
    // in headerHtml() and this palette agree on the active theme's primary.
    {
        const ThemeColors c = themeColors();
        QPalette pal = header->palette();
        pal.setColor(QPalette::WindowText, QColor(c.primary));
        pal.setColor(QPalette::Text,       QColor(c.primary));
        header->setPalette(pal);
    }
    header->setText(headerHtml(m, /*full=*/initiallyExpanded));

    // Per-card Gmail-web-style action row: a Reply icon button + a
    // 3-dot overflow menu sit on the right side of the header. The
    // header label takes the stretch on the left; buttons hug the
    // right. Mirrors mail.google.com's per-message card chrome.
    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(4);
    headerRow->addWidget(header, /*stretch=*/1);

    auto* replyBtn = new QToolButton(card);
    replyBtn->setIcon(IconLoader::themed(QStringLiteral("reply.svg")));
    replyBtn->setIconSize(QSize(16, 16));
    replyBtn->setAutoRaise(true);
    replyBtn->setCursor(Qt::PointingHandCursor);
    replyBtn->setToolTip(tr("Reply"));
    replyBtn->setAccessibleName(tr("Reply to this message"));
    {
        const QString mid = m.id;
        QObject::connect(replyBtn, &QToolButton::clicked, this,
            [this, mid] { emit replyToMessageRequested(mid); });
    }
    headerRow->addWidget(replyBtn);

    auto* moreBtn = new QToolButton(card);
    moreBtn->setIcon(IconLoader::themed(QStringLiteral("menu.svg")));
    moreBtn->setIconSize(QSize(16, 16));
    moreBtn->setAutoRaise(true);
    moreBtn->setCursor(Qt::PointingHandCursor);
    moreBtn->setPopupMode(QToolButton::InstantPopup);
    moreBtn->setToolTip(tr("More actions"));
    moreBtn->setAccessibleName(tr("More actions for this message"));
    auto* moreMenu = new QMenu(moreBtn);
    {
        const QString mid = m.id;
        const bool isUnread = m.isUnread;

        auto* aReplyAll = moreMenu->addAction(tr("Reply all"));
        QObject::connect(aReplyAll, &QAction::triggered, this,
            [this, mid] { emit replyAllToMessageRequested(mid); });

        auto* aForward  = moreMenu->addAction(tr("Forward"));
        QObject::connect(aForward, &QAction::triggered, this,
            [this, mid] { emit forwardMessageRequested(mid); });

        moreMenu->addSeparator();

        auto* aArchive = moreMenu->addAction(tr("Archive"));
        QObject::connect(aArchive, &QAction::triggered, this,
            [this, mid] { emit archiveMessageRequested(mid); });

        // Gmail web shows "Mark unread / Mark read" depending on state;
        // mirror that.
        auto* aRead = moreMenu->addAction(
            isUnread ? tr("Mark read") : tr("Mark unread"));
        QObject::connect(aRead, &QAction::triggered, this,
            [this, mid, isUnread] {
                emit markMessageReadRequested(mid, /*read=*/isUnread);
            });

        auto* aSnooze = moreMenu->addAction(tr("Snooze"));
        QObject::connect(aSnooze, &QAction::triggered, this,
            [this, mid] { emit snoozeMessageRequested(mid); });

        moreMenu->addSeparator();

        auto* aDelete = moreMenu->addAction(tr("Delete this message"));
        QObject::connect(aDelete, &QAction::triggered, this,
            [this, mid] { emit deleteMessageRequested(mid); });
    }
    moreBtn->setMenu(moreMenu);
    headerRow->addWidget(moreBtn);

    cardLayout->addLayout(headerRow);

    auto* body = new AutoSizeTextBrowser(card);
    // Trim the QTextDocument's default 4-pt margin: combined with the
    // card padding it added enough whitespace around the message body
    // that emails felt floaty rather than dense.
    body->document()->setDocumentMargin(2);
    // Route external-link clicks through util::launchBrowser instead of
    // Qt's default QDesktopServices::openUrl. The default path on Linux
    // ends up calling xdg-open, which on WSL ends up calling wslview,
    // which on user setups with /mnt/c missing or partially mounted just
    // silently fails. Our launcher chain has the WSL/xdg-open quirks
    // baked in and falls through to direct browser names.
    body->setOpenExternalLinks(false);
    body->setOpenLinks(false);
    QObject::connect(body, &QTextBrowser::anchorClicked, body,
                     [](const QUrl& clicked) {
                         if (!clicked.isValid()) return;
                         fc::util::launchBrowser(clicked);
                     });
    // Forward link-hover events up so MainWindow can show the URL
    // in the status bar — QTextBrowser's `highlighted` fires with
    // the link's URL on enter and an empty/invalid QUrl on leave.
    // We pass the link target through unchanged; the title="…"
    // attribute we attached in linkifyPlainText is what carries
    // the real destination for "label [URL]" patterns.
    QObject::connect(body, &QTextBrowser::highlighted, this,
                     [this](const QUrl& u) {
                         emit urlHovered(u.isValid() ? u.toString() : QString());
                     });
    body->setReadOnly(true);
    body->setFrameShape(QFrame::NoFrame);
    body->setStyleSheet(QStringLiteral("background: transparent;"));
    // Horizontally fill the card; vertically size to the document via
    // AutoSizeTextBrowser::heightForWidth. No more 360-px floor — short
    // replies in a thread now render at their natural one- or two-line
    // height instead of stretching out a huge empty box.
    body->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    body->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    body->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Defensive: explicitly pin link color on the body's palette so dark mode
    // doesn't render <a href> anchors in the unreadable Fusion-default blue.
    // This matches what Theme::apply pins on the application palette.
    {
        const bool dark = Theme::resolveMode(Theme::currentMode())
                           == Theme::Mode::Dark;
        QPalette pal = body->palette();
        pal.setColor(QPalette::Link,
                     QColor(dark ? QStringLiteral("#8ab4f8")
                                 : QStringLiteral("#1a73e8")));
        pal.setColor(QPalette::LinkVisited,
                     QColor(dark ? QStringLiteral("#c58af9")
                                 : QStringLiteral("#7c4dff")));
        body->setPalette(pal);
    }
    body->setHtml(bodyHtml(m));
    body->setVisible(initiallyExpanded);

    // "Open in browser" rows. We render TWO copies — above and below the
    // body — so the link is reachable on long messages without scrolling.
    // Each row carries three buttons:
    //   - "Open in browser"   strict CSP, no remote loads
    //   - "Open with images"  proxy-rewritten <img>, CSP locked to the
    //                          proxy host only — neither marketer's CDN
    //                          nor any other host can be reached
    //   - "Open in Gmail"     mail.google.com URL for the same thread,
    //                          a clean fallback if our render misbehaves
    // Strict and proxy modes share LocalHtmlServer holders so a re-click
    // reuses the same server / URL token.
    const auto previewMode = Preferences::htmlPreview();
    // The button row appears whenever the message would have ANY
    // useful action available. "Open in Gmail" is universal — it
    // doesn't render HTML locally, just opens the thread on
    // mail.google.com — so we want it reachable even when the user
    // explicitly disabled HTML preview. The strict / images openers
    // ARE local-rendering paths, so those still respect the
    // previewMode == Disabled choice and stay disabled (with a
    // tooltip) in that mode.
    const bool offerBrowserButtons = true;
    const bool hasHtml = !m.bodyHtml.isEmpty() || m.bodyHtmlPresent;
    const bool localPreviewOk = previewMode != Preferences::HtmlPreview::Disabled;
    auto srvHolderStrict = std::make_shared<QPointer<LocalHtmlServer>>();
    auto srvHolderImages = std::make_shared<QPointer<LocalHtmlServer>>();
    auto htmlForServer = m.bodyHtml.isEmpty()
        ? QStringLiteral("<p><i>(no HTML body cached)</i></p>")
        : m.bodyHtml;
    const QString threadIdForGmail = m.threadId;

    auto buildBrowserRow = [&]() -> QWidget* {
        // Wrap the QHBoxLayout in a QWidget so the toggle below can
        // show/hide the whole strip in one call. Layouts can't be
        // hidden directly without iterating their children.
        auto* host = new QWidget(card);
        auto* row  = new QHBoxLayout(host);
        row->setContentsMargins(0, 0, 0, 0);
        row->addStretch(1);

        auto* strictBtn = new QPushButton(QObject::tr("Open in browser"), host);
        strictBtn->setObjectName(QStringLiteral("link"));
        strictBtn->setCursor(Qt::PointingHandCursor);
        strictBtn->setToolTip(!localPreviewOk
            ? QObject::tr(
                "HTML preview is disabled in Settings — "
                "set HTML preview to \"Open in external browser\" to "
                "enable this.")
            : hasHtml
                ? QObject::tr(
                    "Render in your system browser. Strict CSP — no remote "
                    "images, no scripts, no iframes.")
                : QObject::tr(
                    "Plain-text message — no HTML body to render."));
        strictBtn->setEnabled(localPreviewOk && hasHtml);
        row->addWidget(strictBtn);

        auto* imagesBtn = new QPushButton(QObject::tr("Open with images"), card);
        imagesBtn->setObjectName(QStringLiteral("link"));
        imagesBtn->setCursor(Qt::PointingHandCursor);
        imagesBtn->setToolTip(!localPreviewOk
            ? QObject::tr(
                "HTML preview is disabled in Settings — "
                "set HTML preview to \"Open in external browser\" to "
                "enable this.")
            : hasHtml
                ? QObject::tr(
                    "Same as 'Open in browser' but image URLs are routed through "
                    "the configured image proxy (Settings → HTML preview) so the "
                    "marketer's CDN never sees your IP. Scripts / iframes / forms "
                    "still blocked.")
                : QObject::tr(
                    "Plain-text message — no HTML body to render."));
        imagesBtn->setEnabled(localPreviewOk && hasHtml);
        row->addWidget(imagesBtn);

        auto* gmailBtn = new QPushButton(QObject::tr("Open in Gmail"), card);
        gmailBtn->setObjectName(QStringLiteral("link"));
        gmailBtn->setCursor(Qt::PointingHandCursor);
        gmailBtn->setToolTip(QObject::tr(
            "Open this conversation directly on mail.google.com. Useful "
            "when our local rendering doesn't agree with the email."));
        row->addWidget(gmailBtn);

        // Strict-mode opener: serves m.bodyHtml as-is with no img-src
        // additions, so the browser can't fetch any remote resource.
        QObject::connect(strictBtn, &QPushButton::clicked, card,
            [card, strictBtn, htmlForServer, srvHolderStrict]() {
                LocalHtmlServer* srv = srvHolderStrict->data();
                if (!srv) {
                    srv = new LocalHtmlServer(htmlForServer.toUtf8(),
                                               /*imgSrcAdditions=*/QString(),
                                               card);
                    if (!srv->start()) {
                        strictBtn->setText(
                            QObject::tr("Couldn't start local server"));
                        srv->deleteLater();
                        return;
                    }
                    *srvHolderStrict = srv;
                    QObject::connect(srv, &LocalHtmlServer::expired, card,
                        [srvHolderStrict] { srvHolderStrict->clear(); });
                }
                qInfo("LocalHtmlServer (strict): %s",
                      qUtf8Printable(srv->url().toString()));
                fc::util::launchBrowser(srv->url());
            });

        // Image-proxy opener: rewrite every <img src> to route through
        // the user-configured proxy, then lock CSP to that proxy's
        // origin so a stray un-rewritten URL still can't leak.
        QObject::connect(imagesBtn, &QPushButton::clicked, card,
            [card, imagesBtn, htmlForServer, srvHolderImages]() {
                LocalHtmlServer* srv = srvHolderImages->data();
                if (!srv) {
                    const QString pattern = Preferences::imageProxyUrlPattern();
                    const bool stripPx = Preferences::stripTrackingPixels();
                    const QString rewritten = fc::util::rewriteImagesForBrowser(
                        htmlForServer, pattern, stripPx);

                    // Compute the CSP addition: scheme + host of the
                    // proxy URL. If the pattern is malformed we fall
                    // back to allowing https: in general — degrades
                    // privacy a hair but keeps the page readable.
                    QString cspAdd;
                    QString proxySample = pattern;
                    proxySample.replace(QStringLiteral("{url}"),
                                         QStringLiteral("about:blank"));
                    const QUrl proxyUrl(proxySample);
                    if (proxyUrl.isValid() && !proxyUrl.host().isEmpty()) {
                        cspAdd = proxyUrl.scheme() + QStringLiteral("://")
                               + proxyUrl.host() + QStringLiteral("/");
                    } else {
                        cspAdd = QStringLiteral("https:");
                    }

                    srv = new LocalHtmlServer(rewritten.toUtf8(),
                                               cspAdd, card);
                    if (!srv->start()) {
                        imagesBtn->setText(
                            QObject::tr("Couldn't start local server"));
                        srv->deleteLater();
                        return;
                    }
                    *srvHolderImages = srv;
                    QObject::connect(srv, &LocalHtmlServer::expired, card,
                        [srvHolderImages] { srvHolderImages->clear(); });
                }
                qInfo("LocalHtmlServer (proxy): %s",
                      qUtf8Printable(srv->url().toString()));
                fc::util::launchBrowser(srv->url());
            });

        // Gmail-web opener: build the mail.google.com URL. Uses the
        // signed-in account's email as the authuser hint so Google
        // routes the request to the right session even if the user
        // has multiple accounts logged in.
        QObject::connect(gmailBtn, &QPushButton::clicked, card,
            [threadIdForGmail]() {
                if (threadIdForGmail.isEmpty()) return;
                QString email = fc::cache::MetaRepository::get(
                    QStringLiteral("email"));
                QString url = QStringLiteral("https://mail.google.com/mail/");
                if (!email.isEmpty()) {
                    url += QStringLiteral("?authuser=")
                         + QString::fromLatin1(QUrl::toPercentEncoding(email));
                }
                url += QStringLiteral("#all/") + threadIdForGmail;
                qInfo("Opening Gmail web: %s", qUtf8Printable(url));
                fc::util::launchBrowser(QUrl(url));
            });

        return host;
    };

    QWidget* topBrowserRow = nullptr;
    if (offerBrowserButtons) {
        topBrowserRow = buildBrowserRow();
        topBrowserRow->setVisible(initiallyExpanded);
        cardLayout->addWidget(topBrowserRow);
    }

    // No vertical stretch: AutoSizeTextBrowser reports the exact height
    // the document needs, so giving it stretch=1 would just stretch it
    // past content and re-introduce the empty-box problem.
    cardLayout->addWidget(body);

    // Attachment chips. One QPushButton per attachment, click → emit a
    // download request that MainWindow bridges to GmailClient::getAttachment.
    // Hidden when collapsed (matches body visibility) so the chip strip
    // doesn't dangle under a header-only card. The Gmail attachmentId can be
    // empty for inline parts that we synthesised a row id for; in that case
    // the chip is informational only — not downloadable on its own — and
    // is rendered disabled.
    if (initiallyExpanded && !m.attachments.empty()) {
        auto* attachRow = new QHBoxLayout;
        attachRow->setSpacing(6);

        // Count downloadable attachments — the "Download all" button only
        // makes sense when at least two are real attachmentId-backed parts.
        int downloadable = 0;
        for (const auto& a : m.attachments) if (!a.id.isEmpty()) ++downloadable;

        for (const auto& a : m.attachments) {
            const QString sizeStr = humanSize(a.size);
            QString label = a.filename.isEmpty()
                ? QObject::tr("(unnamed)")
                : a.filename;
            if (!sizeStr.isEmpty()) label += QStringLiteral("  ·  ") + sizeStr;

            // IconLoader::themed re-renders the SVG in the active theme's
            // foreground colour; otherwise the icon is invisible black-
            // on-dark in dark mode.
            auto* chip = new QPushButton(
                IconLoader::themed(QStringLiteral("paperclip.svg")),
                label, card);
            chip->setObjectName(QStringLiteral("attachmentChip"));
            chip->setCursor(Qt::PointingHandCursor);
            chip->setToolTip(QObject::tr("Open %1 (%2) — right-click to save")
                                .arg(a.filename, a.mimeType));
            // Right-click context menu: Save as… (forces a picker even when
            // "always ask" is off).
            chip->setContextMenuPolicy(Qt::CustomContextMenu);
            // Inline parts (signature images, embedded chains) come back from
            // Gmail with body.attachmentId blank — only the message-level
            // attachmentId is downloadable. Disable the chip in that case
            // rather than firing a no-op request.
            if (a.id.isEmpty()) chip->setEnabled(false);
            const QString messageId    = m.id;
            const QString attachmentId = a.id;
            const QString filename     = a.filename;
            // Left-click: open in a temp file (no permanent save).
            QObject::connect(chip, &QPushButton::clicked, this,
                [this, messageId, attachmentId, filename]() {
                    emit openAttachmentRequested(
                        messageId, attachmentId, filename);
                });
            // Right-click: Save as… (file picker, default initial dir is
            // Preferences::attachmentDir()).
            QObject::connect(chip, &QWidget::customContextMenuRequested, this,
                [this, chip, messageId, attachmentId, filename](const QPoint& pos) {
                    if (attachmentId.isEmpty()) return;
                    QMenu menu(chip);
                    auto* saveAs = menu.addAction(QObject::tr("Save as…"));
                    QObject::connect(saveAs, &QAction::triggered, this,
                        [this, messageId, attachmentId, filename]() {
                            emit saveAsAttachmentRequested(
                                messageId, attachmentId, filename);
                        });
                    menu.exec(chip->mapToGlobal(pos));
                });
            attachRow->addWidget(chip);
        }

        if (downloadable >= 2) {
            auto* allBtn = new QPushButton(
                IconLoader::themed(QStringLiteral("paperclip.svg")),
                QObject::tr("Download all"), card);
            allBtn->setObjectName(QStringLiteral("attachmentChip"));
            allBtn->setCursor(Qt::PointingHandCursor);
            const QString messageId = m.id;
            QObject::connect(allBtn, &QPushButton::clicked, this,
                [this, messageId]() { emit downloadAllRequested(messageId); });
            attachRow->addWidget(allBtn);
        }

        attachRow->addStretch(1);
        cardLayout->addLayout(attachRow);
    }

    // Mirror of the top-of-card "Open in browser" / "Open with images"
    // row so the link is reachable when the user is at the bottom of a
    // long message. Same shared holders → same servers → same URL/token
    // when re-clicked from either copy.
    QWidget* bottomBrowserRow = nullptr;
    if (offerBrowserButtons) {
        bottomBrowserRow = buildBrowserRow();
        bottomBrowserRow->setVisible(initiallyExpanded);
        cardLayout->addWidget(bottomBrowserRow);
    }

    {
        // Show/Hide toggle on every card — including initially-expanded
        // ones. Without it, the user couldn't collapse a card once it
        // had opened, leading to indefinitely-tall threads. The toggle
        // sits at the bottom-right of the card; its label tracks the
        // current visibility state.
        header->setCursor(Qt::PointingHandCursor);
        QObject::connect(header, &QLabel::linkActivated,
                         body, [](const QString&) { /* let body handle */ });
        auto* toggle = new QPushButton(
            initiallyExpanded ? QStringLiteral("Hide")
                              : QStringLiteral("Show"),
            card);
        toggle->setFlat(true);
        toggle->setCursor(Qt::PointingHandCursor);
        cardLayout->addWidget(toggle, 0, Qt::AlignRight);
        QObject::connect(toggle, &QPushButton::clicked,
                         body, [this, body, header, m, toggle,
                                 topBrowserRow, bottomBrowserRow]() {
            const bool now = !body->isVisible();
            body->setVisible(now);
            if (topBrowserRow)    topBrowserRow->setVisible(now);
            if (bottomBrowserRow) bottomBrowserRow->setVisible(now);
            header->setText(headerHtml(m, /*full=*/now));
            toggle->setText(now ? QStringLiteral("Hide")
                                : QStringLiteral("Show"));
            // Remember the user's choice for this message in this
            // thread. Next time showThread renders the same thread
            // (e.g. the user clicks a different message in the
            // conversation), this card stays in the same state.
            if (now) expandedInCurrentThread_.insert(m.id);
            else      expandedInCurrentThread_.remove(m.id);
        });
    }

    return card;
}

void ReaderPane::showLoading() {
    clearStack();
    auto* l = new QLabel(QStringLiteral("<i>Loading…</i>"), content_);
    l->setTextFormat(Qt::RichText);
    contentLayout_->insertWidget(0, l);
}

void ReaderPane::showEmpty(const QString& reason) {
    clearStack();
    currentThreadId_.clear();
    expandedInCurrentThread_.clear();
    auto* l = new QLabel(reason.isEmpty()
        ? QStringLiteral("<i>Select a message.</i>")
        : QStringLiteral("<i>%1</i>").arg(reason.toHtmlEscaped()), content_);
    l->setTextFormat(Qt::RichText);
    contentLayout_->insertWidget(0, l);
}

void ReaderPane::showMessage(const fc::Message& m) {
    clearStack();
    // Single-message render is its own context; drop any thread-scoped
    // expansion memory so a later showThread starts fresh.
    currentThreadId_.clear();
    expandedInCurrentThread_.clear();
    // The body in each card now sizes to its rendered document height,
    // so we do NOT stretch the card to fill the pane vertically — that
    // would just inflate the body back to a giant empty box for short
    // messages. Insert the card above the trailing stretch (which
    // clearStack just re-added) and let the QScrollArea handle any
    // overflow.
    auto* card = buildMessageCard(m, /*initiallyExpanded=*/true);
    contentLayout_->insertWidget(0, card);
}

void ReaderPane::showThread(const std::vector<fc::Message>& messages,
                              const QString& focusedId) {
    clearStack();
    if (messages.empty()) { showEmpty(); return; }

    // Same-thread re-render? Preserve the user's expanded set; on a
    // different thread, reset. messages all share a threadId (showThread
    // is invoked from byThread() which is single-thread by definition).
    const QString threadId = messages.front().threadId;
    if (threadId != currentThreadId_) {
        expandedInCurrentThread_.clear();
        currentThreadId_ = threadId;
    }

    // Pick which card starts expanded. focusedId wins when it matches
    // one of the thread's messages; otherwise we fall back to "latest"
    // (the last entry, since byThread sorts ascending by date).
    int focusIdx = -1;
    if (!focusedId.isEmpty()) {
        for (int i = 0; i < int(messages.size()); ++i) {
            if (messages[i].id == focusedId) { focusIdx = i; break; }
        }
    }
    if (focusIdx < 0) focusIdx = int(messages.size()) - 1;

    QWidget* focusedCard = nullptr;
    for (int i = 0; i < int(messages.size()); ++i) {
        // Initially expanded if: it's the focused card, OR the user
        // already expanded it in this thread on a prior render.
        const bool expanded = (i == focusIdx)
            || expandedInCurrentThread_.contains(messages[i].id);
        QWidget* card = buildMessageCard(messages[i], expanded);
        if (i == focusIdx) focusedCard = card;
        if (expanded) expandedInCurrentThread_.insert(messages[i].id);
        contentLayout_->insertWidget(i, card);
    }

    // Top-align the focused card: scroll so its top edge sits at the
    // viewport's top edge. Mirrors Gmail web's behaviour when you click
    // a message in the threadlist — the chosen card snaps to the top
    // rather than landing wherever ensureWidgetVisible decides. We
    // wait one event-loop tick because at this synchronous point the
    // cards' y() values are still 0; QScrollArea hasn't laid them out
    // yet. clamp to maximum so we don't try to scroll past the end on
    // very short threads.
    if (focusedCard) {
        QPointer<QScrollArea> s = scroll_;
        QPointer<QWidget> c = focusedCard;
        QTimer::singleShot(0, this, [s, c] {
            if (!s || !c) return;
            auto* bar = s->verticalScrollBar();
            if (!bar) return;
            const int target = qBound(bar->minimum(), c->y(), bar->maximum());
            bar->setValue(target);
        });
    }
}

}  // namespace fc::ui
