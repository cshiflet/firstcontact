#include "ReaderPane.h"

#include "HtmlRenderHostLoader.h"
#include "IHtmlRenderHost.h"
#include "LocalHtmlServer.h"
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
#include <QTextBrowser>
#include <QTextDocument>
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
    QString h = QStringLiteral(
        "<div style='font-weight:600; font-size:13pt; line-height:1.3; color:%1;'>%2</div>"
        "<div style='color:%3; font-size:10pt; margin-top:2px;'>"
        "<span style='font-weight:500; color:%1;'>%4</span> &nbsp;·&nbsp; %5</div>")
        .arg(c.primary,
             subject.isEmpty()
                 ? QStringLiteral("<i>(no subject)</i>")
                 : subject.toHtmlEscaped(),
             c.secondary,
             fromDisplay(m),
             formatDate(m.internalDate));
    if (full) {
        if (!m.toAddrs.isEmpty()) {
            h += QStringLiteral("<div style='color:%1; font-size:9pt; margin-top:4px;'>"
                                "<b>to</b> %2</div>")
                    .arg(c.secondary,
                         m.toAddrs.join(QStringLiteral(", ")).toHtmlEscaped());
        }
        if (!m.ccAddrs.isEmpty()) {
            h += QStringLiteral("<div style='color:%1; font-size:9pt;'>"
                                "<b>cc</b> %2</div>")
                    .arg(c.secondary,
                         m.ccAddrs.join(QStringLiteral(", ")).toHtmlEscaped());
        }
    }
    return h;
}

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

QString bodyHtml(const fc::Message& m) {
    if (!m.bodyText.isEmpty()) return util::linkifyPlainText(m.bodyText);
    if (!m.bodyHtml.isEmpty()) {
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
    cardLayout->addWidget(header);

    auto* body = new QTextBrowser(card);
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
    body->setReadOnly(true);
    body->setFrameShape(QFrame::NoFrame);
    body->setStyleSheet(QStringLiteral("background: transparent;"));
    body->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    body->setMinimumHeight(360);   // floor so the card has a useful default
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
    cardLayout->addWidget(body, /*stretch=*/1);

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

    // Offer "Show full HTML" only when the message actually has HTML and the
    // user hasn't disabled the feature in Preferences. The button does
    // different things depending on the chosen mode.
    const auto previewMode = Preferences::htmlPreview();
    if (initiallyExpanded
        && (!m.bodyHtml.isEmpty() || m.bodyHtmlPresent)
        && previewMode != Preferences::HtmlPreview::Disabled) {
        auto* row = new QHBoxLayout;
        row->addStretch(1);
        auto* webBtn = new QPushButton(
            previewMode == Preferences::HtmlPreview::ExternalBrowser
                ? tr("Open in browser")
                : tr("Show full HTML"), card);
        webBtn->setObjectName(QStringLiteral("link"));
        webBtn->setCursor(Qt::PointingHandCursor);
        row->addWidget(webBtn);
        cardLayout->addLayout(row);

        const QString html = m.bodyHtml.isEmpty()
            ? QStringLiteral("<p><i>(no HTML body cached)</i></p>")
            : m.bodyHtml;

        // Holder for the LocalHtmlServer that survives across button clicks.
        // shared_ptr<QPointer<...>> so the lambda captures by value but
        // assignments are visible across invocations; QPointer auto-clears
        // when the QObject is destroyed (deleteLater on expiry).
        auto srvHolder = std::make_shared<QPointer<LocalHtmlServer>>();

        QObject::connect(webBtn, &QPushButton::clicked, card,
            [card, cardLayout, body, webBtn, html, srvHolder]() {
                const auto mode = Preferences::htmlPreview();

                if (mode == Preferences::HtmlPreview::ExternalBrowser) {
                    // Reuse the prior server if it's still listening — same
                    // URL/token still serves the same HTML — so the user can
                    // close the browser and click again to re-open without
                    // waiting for the lifetime timer to recycle it. If it's
                    // null (never created or already expired), spin a new
                    // one. Either way we leave the button enabled so the
                    // user can re-launch on demand.
                    LocalHtmlServer* srv = srvHolder->data();
                    if (!srv) {
                        srv = new LocalHtmlServer(html.toUtf8(), card);
                        if (!srv->start()) {
                            webBtn->setText(QObject::tr("Couldn't start local server"));
                            srv->deleteLater();
                            return;
                        }
                        *srvHolder = srv;
                        // expired → null out the holder so the next click
                        // creates a fresh server. The button text doesn't
                        // need to change because it never moved off
                        // "Open in browser".
                        QObject::connect(srv, &LocalHtmlServer::expired,
                                         card, [srvHolder] {
                            srvHolder->clear();
                        });
                    }
                    const QUrl u = srv->url();
                    qInfo("LocalHtmlServer: serving HTML at %s",
                          qUtf8Printable(u.toString()));
                    fc::util::launchBrowser(u);
                    return;
                }

                if (mode == Preferences::HtmlPreview::InlineWebEngine
                    && HtmlRenderHostLoader::available()) {
                    auto* host = HtmlRenderHostLoader::create(card);
                    if (!host) {
                        webBtn->setText(QObject::tr("WebEngine plugin not available"));
                        return;
                    }
                    body->hide();
                    webBtn->setEnabled(false);
                    webBtn->setText(QObject::tr("Loading full HTML…"));
                    QWidget* w = host->widget();
                    w->setMinimumHeight(400);
                    cardLayout->addWidget(w);
                    host->render(html, /*allowRemote=*/false);
                    QObject::connect(w, &QWidget::destroyed,
                                     [host] { delete host; });
                    return;
                }

                webBtn->setText(QObject::tr("HTML preview unavailable — see Settings"));
            });
    }

    if (!initiallyExpanded) {
        // Click anywhere on a collapsed card's header to expand.
        header->setCursor(Qt::PointingHandCursor);
        QObject::connect(header, &QLabel::linkActivated,
                         body, [](const QString&) { /* let body handle */ });
        // We don't have a click signal on QLabel; wrap via event filter using
        // a button-like approach: add a small ▾ toggle.
        auto* toggle = new QPushButton(QStringLiteral("Show"), card);
        toggle->setFlat(true);
        toggle->setCursor(Qt::PointingHandCursor);
        cardLayout->addWidget(toggle, 0, Qt::AlignRight);
        QObject::connect(toggle, &QPushButton::clicked,
                         body, [body, header, m, toggle]() {
            const bool now = !body->isVisible();
            body->setVisible(now);
            header->setText(headerHtml(m, /*full=*/now));
            toggle->setText(now ? QStringLiteral("Hide")
                                : QStringLiteral("Show"));
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
    auto* l = new QLabel(reason.isEmpty()
        ? QStringLiteral("<i>Select a message.</i>")
        : QStringLiteral("<i>%1</i>").arg(reason.toHtmlEscaped()), content_);
    l->setTextFormat(Qt::RichText);
    contentLayout_->insertWidget(0, l);
}

void ReaderPane::showMessage(const fc::Message& m) {
    clearStack();
    // Single-message view: drop the trailing stretch so the card fills the
    // pane vertically. The card's body has Expanding size policy so it
    // grows with the available height.
    if (auto* tail = contentLayout_->takeAt(contentLayout_->count() - 1)) {
        delete tail;
    }
    auto* card = buildMessageCard(m, /*initiallyExpanded=*/true);
    contentLayout_->addWidget(card, /*stretch=*/1);
}

void ReaderPane::showThread(const std::vector<fc::Message>& messages) {
    clearStack();
    if (messages.empty()) { showEmpty(); return; }

    // All messages collapsed except the most recent.
    for (int i = 0; i < int(messages.size()); ++i) {
        const bool isLatest = (i == int(messages.size()) - 1);
        contentLayout_->insertWidget(i, buildMessageCard(messages[i], isLatest));
    }
}

}  // namespace fc::ui
