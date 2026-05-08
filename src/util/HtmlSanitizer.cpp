#include "HtmlSanitizer.h"

#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

namespace fc::util {

namespace {

const QSet<QString>& tagAllow() {
    static const QSet<QString> s{
        QStringLiteral("html"), QStringLiteral("body"), QStringLiteral("head"),
        QStringLiteral("title"), QStringLiteral("p"), QStringLiteral("br"),
        QStringLiteral("hr"), QStringLiteral("div"), QStringLiteral("span"),
        QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("strong"),
        QStringLiteral("i"), QStringLiteral("em"), QStringLiteral("u"),
        QStringLiteral("s"), QStringLiteral("strike"), QStringLiteral("sub"),
        QStringLiteral("sup"), QStringLiteral("code"), QStringLiteral("pre"),
        QStringLiteral("blockquote"),
        QStringLiteral("ul"), QStringLiteral("ol"), QStringLiteral("li"),
        QStringLiteral("dl"), QStringLiteral("dt"), QStringLiteral("dd"),
        QStringLiteral("h1"), QStringLiteral("h2"), QStringLiteral("h3"),
        QStringLiteral("h4"), QStringLiteral("h5"), QStringLiteral("h6"),
        QStringLiteral("table"), QStringLiteral("thead"), QStringLiteral("tbody"),
        QStringLiteral("tfoot"), QStringLiteral("tr"), QStringLiteral("td"),
        QStringLiteral("th"), QStringLiteral("caption"), QStringLiteral("colgroup"),
        QStringLiteral("col"),
        QStringLiteral("img"),
        QStringLiteral("font"), QStringLiteral("center"),
    };
    return s;
}

// Tags whose entire subtree (incl. content) must be removed.
const QSet<QString>& tagDrop() {
    static const QSet<QString> s{
        QStringLiteral("script"), QStringLiteral("style"),
        QStringLiteral("iframe"), QStringLiteral("frame"),
        QStringLiteral("frameset"), QStringLiteral("object"),
        QStringLiteral("embed"),   QStringLiteral("applet"),
        QStringLiteral("form"),    QStringLiteral("input"),
        QStringLiteral("button"),  QStringLiteral("textarea"),
        QStringLiteral("select"),  QStringLiteral("option"),
        QStringLiteral("link"),    QStringLiteral("meta"),
        QStringLiteral("base"),    QStringLiteral("svg"),
        QStringLiteral("math"),    QStringLiteral("noscript"),
    };
    return s;
}

const QSet<QString>& voidTags() {
    static const QSet<QString> s{
        QStringLiteral("br"), QStringLiteral("hr"), QStringLiteral("img"),
        QStringLiteral("col"),
    };
    return s;
}

const QSet<QString>& attrAllow() {
    static const QSet<QString> s{
        QStringLiteral("href"), QStringLiteral("src"),
        QStringLiteral("alt"),  QStringLiteral("title"),
        QStringLiteral("width"),QStringLiteral("height"),
        QStringLiteral("colspan"), QStringLiteral("rowspan"),
        QStringLiteral("align"), QStringLiteral("valign"),
        QStringLiteral("border"), QStringLiteral("cellspacing"),
        QStringLiteral("cellpadding"),
    };
    return s;
}

bool isSafeUrl(QString url) {
    url = url.trimmed();
    if (url.isEmpty()) return true;
    static const QRegularExpression dangerous(
        QStringLiteral("^\\s*(javascript|vbscript|file|data|about):"),
        QRegularExpression::CaseInsensitiveOption);
    return !dangerous.match(url).hasMatch();
}

bool isRemoteHttpUrl(const QString& url) {
    return url.startsWith(QLatin1String("http://"),  Qt::CaseInsensitive) ||
           url.startsWith(QLatin1String("https://"), Qt::CaseInsensitive);
}

QString attrEscape(const QString& s) {
    QString out = s;
    out.replace(QChar('&'),  QStringLiteral("&amp;"));
    out.replace(QChar('"'),  QStringLiteral("&quot;"));
    out.replace(QChar('<'),  QStringLiteral("&lt;"));
    out.replace(QChar('>'),  QStringLiteral("&gt;"));
    return out;
}

// Parse the inside of a tag (after `<` and before `>`) into name + attrs.
struct ParsedTag {
    bool       isClose = false;
    bool       selfClose = false;
    QString    name;
    QList<QPair<QString, QString>> attrs;
};

ParsedTag parseTag(QStringView s) {
    ParsedTag t;
    int i = 0;
    if (i < s.size() && s[i] == QLatin1Char('/')) { t.isClose = true; ++i; }
    while (i < s.size() && (s[i].isLetter() || s[i].isDigit())) {
        t.name.append(s[i].toLower());
        ++i;
    }

    auto skipWs = [&] {
        while (i < s.size() && s[i].isSpace()) ++i;
    };

    while (true) {
        skipWs();
        if (i >= s.size()) break;
        if (s[i] == QLatin1Char('/')) { t.selfClose = true; ++i; continue; }

        QString aname;
        while (i < s.size() && !s[i].isSpace() && s[i] != QLatin1Char('=') &&
               s[i] != QLatin1Char('>') && s[i] != QLatin1Char('/')) {
            aname.append(s[i].toLower());
            ++i;
        }
        if (aname.isEmpty()) break;

        QString avalue;
        skipWs();
        if (i < s.size() && s[i] == QLatin1Char('=')) {
            ++i;
            skipWs();
            if (i < s.size() && (s[i] == QLatin1Char('"') || s[i] == QLatin1Char('\''))) {
                const QChar quote = s[i];
                ++i;
                while (i < s.size() && s[i] != quote) { avalue.append(s[i]); ++i; }
                if (i < s.size()) ++i;
            } else {
                while (i < s.size() && !s[i].isSpace() &&
                       s[i] != QLatin1Char('>') && s[i] != QLatin1Char('/')) {
                    avalue.append(s[i]);
                    ++i;
                }
            }
        }
        t.attrs.append({aname, avalue});
    }
    return t;
}

}  // namespace

SanitizeResult sanitizeHtml(const QString& dirty, const SanitizeOptions& opts) {
    SanitizeResult res;
    if (dirty.isEmpty()) return res;

    QString out;
    out.reserve(dirty.size());

    int dropDepth = 0;       // we're inside the subtree of a dropped tag
    QString dropName;        // tag whose close ends drop mode

    int i = 0;
    while (i < dirty.size()) {
        // Strip HTML comments.
        if (dirty.mid(i, 4) == QLatin1String("<!--")) {
            const int end = dirty.indexOf(QStringLiteral("-->"), i + 4);
            i = (end < 0) ? dirty.size() : end + 3;
            continue;
        }

        // Strip <!DOCTYPE …> / <?xml … ?>
        if (dirty[i] == QLatin1Char('<') && i + 1 < dirty.size() &&
            (dirty[i + 1] == QLatin1Char('!') || dirty[i + 1] == QLatin1Char('?'))) {
            const int end = dirty.indexOf(QChar('>'), i);
            i = (end < 0) ? dirty.size() : end + 1;
            continue;
        }

        if (dirty[i] != QLatin1Char('<')) {
            if (dropDepth == 0) {
                out.append(dirty[i]);
            }
            ++i;
            continue;
        }

        // Tag. Scan to the matching `>`, but skip over any `>` that
        // sits inside a quoted attribute value — naive indexOf('>')
        // would terminate parsing early on inputs like
        //   <a title="a>b" href="javascript:…">
        // and leak the tail back into the output as escaped text.
        // The result was rendering noise (script never executed —
        // tail still got escaped) but ugly. Walk the input looking
        // for the close while toggling an `inQuote` flag on
        // unescaped `'`/`"`.
        int gt = -1;
        QChar quote;
        for (int j = i + 1; j < dirty.size(); ++j) {
            const QChar c = dirty[j];
            if (!quote.isNull()) {
                if (c == quote) quote = QChar();
                continue;
            }
            if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                quote = c;
                continue;
            }
            if (c == QLatin1Char('>')) { gt = j; break; }
        }
        if (gt < 0) {
            // Unterminated `<`; treat as literal text (escaped).
            if (dropDepth == 0) out.append(QStringLiteral("&lt;"));
            ++i;
            continue;
        }
        const QStringView inner = QStringView{dirty}.mid(i + 1, gt - i - 1);
        const ParsedTag t = parseTag(inner);
        i = gt + 1;
        if (t.name.isEmpty()) continue;

        if (dropDepth > 0) {
            if (t.isClose && t.name == dropName) {
                --dropDepth;
                if (dropDepth == 0) dropName.clear();
            } else if (!t.isClose && t.name == dropName) {
                ++dropDepth;
            }
            continue;
        }

        if (tagDrop().contains(t.name)) {
            if (!t.isClose && !t.selfClose) {
                dropDepth = 1;
                dropName = t.name;
            }
            continue;
        }

        if (!tagAllow().contains(t.name)) {
            // Unknown tag — drop the tag itself but keep nested text.
            continue;
        }

        if (t.isClose) {
            out.append(QStringLiteral("</")).append(t.name).append(QChar('>'));
            continue;
        }

        // Open tag with attributes.
        QString attrsOut;
        for (const auto& [aname, avalue] : t.attrs) {
            if (aname.startsWith(QStringLiteral("on"))) continue;          // event handler
            if (aname == QLatin1String("style")) continue;                 // CSS exploits
            if (!attrAllow().contains(aname))         continue;
            QString value = avalue;
            if (aname == QLatin1String("href") || aname == QLatin1String("src")) {
                if (!isSafeUrl(value)) continue;
                if (t.name == QLatin1String("img") && !opts.allowRemoteImages
                    && isRemoteHttpUrl(value)) {
                    res.remoteImagesBlocked = true;
                    continue;
                }
            }
            attrsOut.append(QChar(' '))
                    .append(aname)
                    .append(QStringLiteral("=\""))
                    .append(attrEscape(value))
                    .append(QChar('"'));
        }

        out.append(QChar('<')).append(t.name).append(attrsOut);
        // HTML5 only treats VOID elements as self-closing when written
        // `<tag />`. For non-void tags an XHTML-style `<div/>` parses
        // as an open `<div>` (with no matching close), which leaves
        // the document tree imbalanced. Honour selfClose only when
        // the element is in our void-tag list.
        if (voidTags().contains(t.name)) {
            out.append(QStringLiteral(" />"));
        } else {
            out.append(QChar('>'));
        }
    }

    res.html = out;
    return res;
}

}  // namespace fc::util
