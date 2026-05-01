#include "ReaderPane.h"

#include "util/HtmlSanitizer.h"
#include "util/Html2Text.h"
#include "util/Linkify.h"

#include <QDateTime>
#include <QLabel>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace fc::ui {

ReaderPane::ReaderPane(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 12);

    headerLabel_ = new QLabel(this);
    headerLabel_->setTextFormat(Qt::RichText);
    headerLabel_->setWordWrap(true);
    headerLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(headerLabel_);

    body_ = new QTextBrowser(this);
    body_->setOpenExternalLinks(true);
    body_->setReadOnly(true);
    layout->addWidget(body_, /*stretch=*/1);

    showEmpty();
}

void ReaderPane::showLoading() {
    headerLabel_->setText(QStringLiteral("<i>Loading…</i>"));
    body_->clear();
}

void ReaderPane::showEmpty(const QString& reason) {
    headerLabel_->setText(reason.isEmpty()
        ? QStringLiteral("<i>Select a message.</i>")
        : QStringLiteral("<i>%1</i>").arg(reason.toHtmlEscaped()));
    body_->clear();
}

void ReaderPane::showMessage(const fc::Message& m) {
    const QString from = m.fromName.isEmpty()
        ? m.fromAddr
        : QStringLiteral("%1 <%2>").arg(m.fromName, m.fromAddr);
    const QString date = QDateTime::fromMSecsSinceEpoch(m.internalDate)
                            .toLocalTime()
                            .toString(QStringLiteral("ddd, MMM d, yyyy h:mm AP"));

    QString header = QStringLiteral(
        "<div style='font-size:13pt;font-weight:bold;'>%1</div>"
        "<div><b>From:</b> %2</div>"
        "<div><b>To:</b> %3</div>")
        .arg(m.subject.toHtmlEscaped(),
             from.toHtmlEscaped(),
             m.toAddrs.join(QStringLiteral(", ")).toHtmlEscaped());
    if (!m.ccAddrs.isEmpty()) {
        header += QStringLiteral("<div><b>Cc:</b> %1</div>")
                    .arg(m.ccAddrs.join(QStringLiteral(", ")).toHtmlEscaped());
    }
    header += QStringLiteral("<div style='color:gray;'>%1</div>").arg(date);
    headerLabel_->setText(header);

    if (!m.bodyText.isEmpty()) {
        body_->setHtml(util::linkifyPlainText(m.bodyText));
    } else if (!m.bodyHtml.isEmpty()) {
        const auto safe = util::sanitizeHtml(m.bodyHtml);
        QString rendered = safe.html;
        if (safe.remoteImagesBlocked) {
            rendered.prepend(QStringLiteral(
                "<div style='background:#fff8c4;padding:6px;border:1px solid #d4d000;'>"
                "<i>Remote images blocked. Showing as plain HTML.</i></div>"));
        }
        body_->setHtml(rendered);
    } else if (m.bodyHtmlPresent) {
        // We know the message has HTML but haven't fetched it; fall back
        // to a notice. Once Phase 4 fetches HTML on demand this branch
        // will trigger an async getMessage(format=full).
        body_->setHtml(QStringLiteral("<p><i>This message has HTML content "
                                      "that hasn't been fetched yet.</i></p>"));
    } else {
        body_->setHtml(QStringLiteral("<p><i>(empty message)</i></p>"));
    }

    if (!m.attachments.empty()) {
        QString att = QStringLiteral("<hr><b>Attachments:</b><ul>");
        for (const auto& a : m.attachments) {
            att += QStringLiteral("<li>%1 (%2 bytes)</li>")
                       .arg(a.filename.toHtmlEscaped()).arg(a.size);
        }
        att += QStringLiteral("</ul>");
        body_->append(att);
    }
}

}  // namespace fc::ui
