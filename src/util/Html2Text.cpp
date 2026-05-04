#include "Html2Text.h"

#include <QRegularExpression>
#include <QString>

namespace fc::util {

namespace {

QString collapseWhitespace(QString s) {
    // Note: PCRE's \v matches vertical whitespace incl. \n, so we use a
    // literal character class instead of relying on \v.
    static const QRegularExpression spaces(QStringLiteral("[ \\t\\r\\f]+"));
    s.replace(spaces, QStringLiteral(" "));
    static const QRegularExpression manyNl(QStringLiteral("\\n{3,}"));
    s.replace(manyNl, QStringLiteral("\n\n"));
    return s.trimmed();
}

QString decodeEntitiesImpl(QString s) {
    // Numeric &#1234; / &#xAB;
    static const QRegularExpression numRe(QStringLiteral(R"(&#(x?)([0-9A-Fa-f]+);)"));
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
        if (ok && cp != 0) out.append(QChar(cp));
        idx = m.capturedEnd();
    }
    out.append(s.mid(idx));
    s = out;

    // Named entities — small whitelist covering >99% of email content.
    static const QPair<QLatin1String, QChar> kNamed[] = {
        {QLatin1String("&nbsp;"),  QChar(0x00A0)},
        {QLatin1String("&lt;"),    QChar('<')},
        {QLatin1String("&gt;"),    QChar('>')},
        {QLatin1String("&quot;"),  QChar('"')},
        {QLatin1String("&apos;"),  QChar('\'')},
        {QLatin1String("&copy;"),  QChar(0x00A9)},
        {QLatin1String("&reg;"),   QChar(0x00AE)},
        {QLatin1String("&trade;"), QChar(0x2122)},
        {QLatin1String("&mdash;"), QChar(0x2014)},
        {QLatin1String("&ndash;"), QChar(0x2013)},
        {QLatin1String("&hellip;"),QChar(0x2026)},
        {QLatin1String("&laquo;"), QChar(0x00AB)},
        {QLatin1String("&raquo;"), QChar(0x00BB)},
        {QLatin1String("&middot;"),QChar(0x00B7)},
        {QLatin1String("&bull;"),  QChar(0x2022)},
    };
    for (const auto& [k, v] : kNamed) s.replace(k, QString(v));
    // &amp; last so we don't double-decode.
    s.replace(QLatin1String("&amp;"), QStringLiteral("&"));
    return s;
}

}  // namespace

QString html2text(const QString& html) {
    if (html.isEmpty()) return {};

    QString s = html;

    // Drop scripts and styles outright.
    static const QRegularExpression dropRe(
        QStringLiteral("<(script|style)[^>]*>.*?</\\1>"),
        QRegularExpression::DotMatchesEverythingOption |
        QRegularExpression::CaseInsensitiveOption);
    s.replace(dropRe, QString());

    // List items get a bullet (must come before the generic block sweep).
    static const QRegularExpression liRe(QStringLiteral("<li\\b[^>]*>"),
                                         QRegularExpression::CaseInsensitiveOption);
    s.replace(liRe, QStringLiteral("\n  • "));

    // Line breaks.
    static const QRegularExpression brRe(QStringLiteral("<br\\s*/?>"),
                                         QRegularExpression::CaseInsensitiveOption);
    s.replace(brRe, QStringLiteral("\n"));

    // Block-level tags become newlines.
    static const QRegularExpression blockRe(
        QStringLiteral("</?(p|div|section|article|header|footer|nav|aside|"
                       "h[1-6]|tr|table|thead|tbody|tfoot|ul|ol|blockquote|"
                       "pre|hr)\\b[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    s.replace(blockRe, QStringLiteral("\n"));

    // Drop everything else.
    static const QRegularExpression tagRe(QStringLiteral("<[^>]+>"));
    s.replace(tagRe, QString());

    s = decodeEntitiesImpl(s);
    return collapseWhitespace(s);
}

QString decodeHtmlEntities(const QString& s) {
    if (s.isEmpty() || !s.contains(QChar('&'))) return s;
    return decodeEntitiesImpl(s);
}

}  // namespace fc::util
