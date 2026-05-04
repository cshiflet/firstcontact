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

// Decode HTML numeric and common named entity references inside a plain-text
// body. Handles &#NNN; (decimal), &#xHH; (hex), and a small whitelist of
// named entities that show up regularly in real-world emails. Anything we
// don't recognise is left alone so genuine "&" + "#" + digits sequences in
// user prose survive.
//
// Why we do this in the linkify path: Gmail's text/plain part frequently
// contains entity references that were never decoded when the HTML→text
// conversion ran on the sender side. The user shouldn't have to look at
// "&#847;" in their inbox.
QString decodeEntitiesInline(QString s) {
    static const QRegularExpression numRe(
        QStringLiteral(R"(&#(x?)([0-9A-Fa-f]+);)"),
        QRegularExpression::CaseInsensitiveOption);
    QString out;
    out.reserve(s.size());
    int idx = 0;
    auto it = numRe.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        out.append(s.mid(idx, m.capturedStart() - idx));
        const bool hex = !m.captured(1).isEmpty();
        bool ok = false;
        const uint cp = m.captured(2).toUInt(&ok, hex ? 16 : 10);
        if (ok && cp != 0 && cp <= 0x10FFFF) {
            out.append(QChar(cp));
        } else {
            out.append(m.captured(0));   // leave malformed alone
        }
        idx = m.capturedEnd();
    }
    out.append(s.mid(idx));
    s = out;

    // Small named-entity table covering >99% of what shows up in plain-text
    // email artefacts. Order matters: &amp; goes LAST so we don't double
    // decode an already-encoded ampersand.
    static const QPair<QLatin1String, QChar> kNamed[] = {
        {QLatin1String("&nbsp;"),   QChar(0x00A0)},
        {QLatin1String("&lt;"),     QChar('<')},
        {QLatin1String("&gt;"),     QChar('>')},
        {QLatin1String("&quot;"),   QChar('"')},
        {QLatin1String("&apos;"),   QChar('\'')},
        {QLatin1String("&copy;"),   QChar(0x00A9)},
        {QLatin1String("&reg;"),    QChar(0x00AE)},
        {QLatin1String("&trade;"),  QChar(0x2122)},
        {QLatin1String("&mdash;"),  QChar(0x2014)},
        {QLatin1String("&ndash;"),  QChar(0x2013)},
        {QLatin1String("&hellip;"), QChar(0x2026)},
        {QLatin1String("&laquo;"),  QChar(0x00AB)},
        {QLatin1String("&raquo;"),  QChar(0x00BB)},
        {QLatin1String("&middot;"), QChar(0x00B7)},
        {QLatin1String("&bull;"),   QChar(0x2022)},
        {QLatin1String("&rsquo;"),  QChar(0x2019)},
        {QLatin1String("&lsquo;"),  QChar(0x2018)},
        {QLatin1String("&rdquo;"),  QChar(0x201D)},
        {QLatin1String("&ldquo;"),  QChar(0x201C)},
    };
    for (const auto& [k, v] : kNamed) s.replace(k, QString(v));
    s.replace(QLatin1String("&amp;"), QStringLiteral("&"));
    return s;
}

}  // namespace

QString linkifyPlainText(const QString& plain) {
    // Decode entity references BEFORE HTML-escaping so that &#847; etc. turn
    // into the actual Unicode character instead of being shown literally.
    QString escaped = decodeEntitiesInline(plain).toHtmlEscaped();

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
