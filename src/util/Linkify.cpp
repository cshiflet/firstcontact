#include "Linkify.h"

#include <QRegularExpression>
#include <QString>

namespace fc::util {

namespace {

// Conservative URL matcher: http(s)://… or www.… up to whitespace / closing
// punctuation. Trailing ).,;!? trimmed below.
const QRegularExpression& urlRe() {
    static const QRegularExpression re(
        QStringLiteral(R"((https?://[^\s<>"']+|www\.[^\s<>"']+))"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

const QRegularExpression& mailRe() {
    static const QRegularExpression re(
        QStringLiteral(R"(([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}))"));
    return re;
}

QString trimTrailingPunct(QString s) {
    while (!s.isEmpty()) {
        const QChar c = s.back();
        if (c == ')' || c == ',' || c == '.' || c == ';' ||
            c == '!' || c == '?' || c == ']' || c == '}') {
            s.chop(1);
        } else {
            break;
        }
    }
    return s;
}

}  // namespace

QString linkifyPlainText(const QString& plain) {
    QString escaped = plain.toHtmlEscaped();

    // Linkify URLs first.
    QString out;
    out.reserve(escaped.size() + 64);
    int idx = 0;
    auto it = urlRe().globalMatch(escaped);
    while (it.hasNext()) {
        const auto m = it.next();
        out.append(escaped.mid(idx, m.capturedStart() - idx));
        QString url = trimTrailingPunct(m.captured(0));
        const int trimmedLen = m.capturedLength(0) - (m.captured(0).size() - url.size());
        QString href = url;
        if (href.startsWith(QStringLiteral("www."), Qt::CaseInsensitive)) {
            href.prepend(QStringLiteral("https://"));
        }
        out.append(QStringLiteral("<a href=\"%1\">%2</a>").arg(href, url));
        idx = m.capturedStart() + trimmedLen;
    }
    out.append(escaped.mid(idx));

    // Then bare emails. Operate on the result so we don't double-wrap URLs.
    QString final;
    final.reserve(out.size() + 64);
    idx = 0;
    auto mit = mailRe().globalMatch(out);
    while (mit.hasNext()) {
        const auto m = mit.next();
        // Skip matches inside an existing href (cheap heuristic).
        const int contextStart = qMax(0, m.capturedStart() - 16);
        if (QStringView{out}.mid(contextStart, m.capturedStart() - contextStart)
                .contains(QStringLiteral("href="))) {
            continue;
        }
        final.append(out.mid(idx, m.capturedStart() - idx));
        final.append(QStringLiteral("<a href=\"mailto:%1\">%1</a>").arg(m.captured(0)));
        idx = m.capturedEnd();
    }
    final.append(out.mid(idx));

    final.replace(QChar('\n'), QStringLiteral("<br>"));
    return final;
}

}  // namespace fc::util
