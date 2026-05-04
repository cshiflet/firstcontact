#include "MessageParser.h"

#include "util/Base64Url.h"
#include "util/Html2Text.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace fc::api {

namespace {

QString header(const QJsonArray& hs, const char* name) {
    for (const auto v : hs) {
        const auto h = v.toObject();
        if (h.value(QStringLiteral("name")).toString().compare(
                QString::fromLatin1(name), Qt::CaseInsensitive) == 0) {
            return h.value(QStringLiteral("value")).toString();
        }
    }
    return {};
}

// Splits "Christopher Shiflet <chris@shiflet.org>" into name + addr.
std::pair<QString, QString> splitAddress(const QString& v) {
    const int lt = v.indexOf('<');
    const int gt = v.indexOf('>');
    if (lt > 0 && gt > lt) {
        QString name = v.left(lt).trimmed();
        if (name.startsWith('"') && name.endsWith('"') && name.size() >= 2) {
            name = name.mid(1, name.size() - 2);
        }
        // Marketing tools sometimes pre-encode display names in HTML
        // entities ("Bob &amp; Co"). Decode now so the cache and FTS see
        // the literal characters.
        return {fc::util::decodeHtmlEntities(name), v.mid(lt + 1, gt - lt - 1).trimmed()};
    }
    return {{}, v.trimmed()};
}

QStringList splitAddressList(const QString& raw) {
    QStringList out;
    int depth = 0;
    QString cur;
    for (const QChar c : raw) {
        if (c == '<') ++depth;
        if (c == '>') --depth;
        if (c == ',' && depth == 0) {
            out << cur.trimmed();
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.trimmed().isEmpty()) out << cur.trimmed();
    return out;
}

void walkPart(const QJsonObject& part, fc::Message& msg) {
    const QString mime = part.value(QStringLiteral("mimeType")).toString();
    const auto body = part.value(QStringLiteral("body")).toObject();
    const QString filename = part.value(QStringLiteral("filename")).toString();

    if (mime.startsWith(QStringLiteral("multipart/"))) {
        for (const auto v : part.value(QStringLiteral("parts")).toArray()) {
            walkPart(v.toObject(), msg);
        }
        return;
    }

    if (!filename.isEmpty()) {
        fc::Attachment a;
        a.id        = body.value(QStringLiteral("attachmentId")).toString();
        a.filename  = filename;
        a.mimeType  = mime;
        a.size      = body.value(QStringLiteral("size")).toInt();
        msg.attachments.push_back(std::move(a));
        msg.hasAttachment = true;
        return;
    }

    if (mime == QLatin1String("text/plain") && msg.bodyText.isEmpty()) {
        const QByteArray raw = body.value(QStringLiteral("data")).toString().toLatin1();
        msg.bodyText = QString::fromUtf8(util::base64UrlDecode(raw));
    } else if (mime == QLatin1String("text/html")) {
        msg.bodyHtmlPresent = true;
        if (msg.bodyHtml.isEmpty()) {
            const QByteArray raw = body.value(QStringLiteral("data")).toString().toLatin1();
            msg.bodyHtml = QString::fromUtf8(util::base64UrlDecode(raw));
        }
    }
}

}  // namespace

fc::Message MessageParser::parse(const QJsonObject& g) {
    fc::Message m;
    m.id            = g.value(QStringLiteral("id")).toString();
    m.threadId      = g.value(QStringLiteral("threadId")).toString();
    m.historyId     = g.value(QStringLiteral("historyId")).toString();
    // Gmail returns the snippet HTML-decoded most of the time, but some
    // forwarded / mailing-list messages slip through with literal numeric
    // entities ("It&#39;s here"). Decode defensively.
    m.snippet       = fc::util::decodeHtmlEntities(
                          g.value(QStringLiteral("snippet")).toString());
    m.internalDate  = g.value(QStringLiteral("internalDate")).toString().toLongLong();
    m.sizeEstimate  = g.value(QStringLiteral("sizeEstimate")).toInt();

    for (const auto v : g.value(QStringLiteral("labelIds")).toArray()) {
        const QString id = v.toString();
        m.labelIds << id;
        if (id == QLatin1String("UNREAD"))    m.isUnread   = true;
        if (id == QLatin1String("STARRED"))   m.isStarred  = true;
        if (id == QLatin1String("IMPORTANT")) m.isImportant = true;
    }

    const auto payload = g.value(QStringLiteral("payload")).toObject();
    const auto headers = payload.value(QStringLiteral("headers")).toArray();

    // Subject lines from marketing senders frequently contain literal HTML
    // entities (&#39;, &amp;, …) because the same template engine
    // generates the body and the headers. Decode at ingest so the cache,
    // FTS index, and every reader render the literal characters.
    m.subject  = fc::util::decodeHtmlEntities(header(headers, "Subject"));
    auto from  = splitAddress(header(headers, "From"));
    m.fromName = from.first;
    m.fromAddr = from.second;
    m.replyTo  = header(headers, "Reply-To");
    m.toAddrs  = splitAddressList(header(headers, "To"));
    m.ccAddrs  = splitAddressList(header(headers, "Cc"));
    m.bccAddrs = splitAddressList(header(headers, "Bcc"));

    walkPart(payload, m);
    return m;
}

}  // namespace fc::api
