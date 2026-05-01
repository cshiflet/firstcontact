#include "ReaderPane.h"

#include "HtmlRenderHostLoader.h"
#include "IHtmlRenderHost.h"
#include "ui/common/Theme.h"
#include "util/HtmlSanitizer.h"
#include "util/Html2Text.h"
#include "util/Linkify.h"

#include <QDateTime>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace fc::ui {

namespace {

QString formatDate(qint64 ms) {
    if (ms <= 0) return {};
    return QDateTime::fromMSecsSinceEpoch(ms).toLocalTime()
        .toString(QStringLiteral("ddd, MMM d, yyyy h:mm AP"));
}

QString fromDisplay(const fc::Message& m) {
    if (!m.fromName.isEmpty() && !m.fromAddr.isEmpty())
        return QStringLiteral("%1 &lt;%2&gt;")
            .arg(m.fromName.toHtmlEscaped(), m.fromAddr.toHtmlEscaped());
    return m.fromAddr.toHtmlEscaped();
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
            QStringLiteral("#e8eaed"),  // primary text
            QStringLiteral("#9aa0a6"),  // secondary text
            QStringLiteral("#3a341a"),  // warning bg (dim amber)
            QStringLiteral("#5e5021"),  // warning border
            QStringLiteral("#fdd663"),  // warning text
        };
    }
    return {
        QStringLiteral("#202124"),
        QStringLiteral("#5f6368"),
        QStringLiteral("#fff8c4"),
        QStringLiteral("#d4d000"),
        QStringLiteral("#5e5500"),
    };
}

QString headerHtml(const fc::Message& m, bool full) {
    const ThemeColors c = themeColors();
    QString h = QStringLiteral(
        "<div style='font-weight:600; font-size:13pt; line-height:1.3; color:%1;'>%2</div>"
        "<div style='color:%3; font-size:10pt; margin-top:2px;'>"
        "<span style='font-weight:500; color:%1;'>%4</span> &nbsp;·&nbsp; %5</div>")
        .arg(c.primary,
             m.subject.isEmpty()
                 ? QStringLiteral("<i>(no subject)</i>")
                 : m.subject.toHtmlEscaped(),
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
    contentLayout_->setContentsMargins(28, 24, 28, 28);
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
    cardLayout->setContentsMargins(20, 16, 20, 16);
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
    header->setText(headerHtml(m, /*full=*/initiallyExpanded));
    cardLayout->addWidget(header);

    auto* body = new QTextBrowser(card);
    body->setOpenExternalLinks(true);
    body->setReadOnly(true);
    body->setFrameShape(QFrame::NoFrame);
    body->setStyleSheet(QStringLiteral("background: transparent;"));
    body->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    body->setMinimumHeight(360);   // floor so the card has a useful default
    body->setHtml(bodyHtml(m));
    body->setVisible(initiallyExpanded);
    cardLayout->addWidget(body, /*stretch=*/1);

    // Offer "Show full HTML" only for messages where the message has HTML
    // and the optional QtWebEngine plugin is deployed.
    if (initiallyExpanded
        && (!m.bodyHtml.isEmpty() || m.bodyHtmlPresent)
        && HtmlRenderHostLoader::available()) {
        auto* row = new QHBoxLayout;
        row->addStretch(1);
        auto* webBtn = new QPushButton(tr("Show full HTML"), card);
        webBtn->setFlat(true);
        webBtn->setCursor(Qt::PointingHandCursor);
        row->addWidget(webBtn);
        cardLayout->addLayout(row);

        const QString html = m.bodyHtml.isEmpty()
            ? QStringLiteral("<p><i>(no HTML body cached)</i></p>")
            : m.bodyHtml;

        QObject::connect(webBtn, &QPushButton::clicked, card,
            [card, cardLayout, body, webBtn, html]() {
                auto* host = HtmlRenderHostLoader::create(card);
                if (!host) return;
                body->hide();
                webBtn->setEnabled(false);
                webBtn->setText(QObject::tr("Loading full HTML…"));
                QWidget* w = host->widget();
                w->setMinimumHeight(400);
                cardLayout->addWidget(w);
                host->render(html, /*allowRemote=*/false);
                // Tie the host's lifetime to its widget so a new message
                // selection (which destroys the card) tears down the
                // off-the-record QWebEngineProfile too.
                QObject::connect(w, &QWidget::destroyed, [host] { delete host; });
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
