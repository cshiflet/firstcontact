#include "MimeBuilder.h"

#include <QByteArray>
#include <QDateTime>
#include <QLocale>
#include <QString>
#include <QUuid>

namespace fc::util {

namespace {

// Strip CR/LF (and other control chars below SP) from a single header field
// value to defeat email header injection. Without this an attacker who
// controls a recipient or subject could splice in extra headers (e.g. Bcc).
QString sanitizeHeaderField(QString s) {
    s.remove(QChar('\r'));
    s.remove(QChar('\n'));
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        if (c.unicode() < 0x20 && c != QChar('\t')) continue;
        out.append(c);
    }
    return out;
}

QString quoteIfNeeded(const QString& name) {
    static const QString specials = QStringLiteral("()<>[]:;@\\,.\"");
    for (const QChar c : name) {
        if (specials.contains(c)) {
            QString escaped = name;
            escaped.replace(QChar('\\'), QStringLiteral("\\\\"));
            escaped.replace(QChar('"'),  QStringLiteral("\\\""));
            return QStringLiteral("\"%1\"").arg(escaped);
        }
    }
    return name;
}

bool needsEncodedWord(const QString& s) {
    for (const QChar c : s) {
        if (c.unicode() > 0x7e || c.unicode() < 0x20) return true;
    }
    return false;
}

QString encodedWord(const QString& s) {
    if (!needsEncodedWord(s)) return s;
    return QStringLiteral("=?UTF-8?B?%1?=").arg(
        QString::fromLatin1(s.toUtf8().toBase64()));
}

QString formatAddress(const QString& addr, const QString& name = {}) {
    const QString safeAddr = sanitizeHeaderField(addr);
    if (name.isEmpty()) return safeAddr;
    const QString safeName = sanitizeHeaderField(name);
    const QString display = needsEncodedWord(safeName)
                                ? encodedWord(safeName)
                                : quoteIfNeeded(safeName);
    return QStringLiteral("%1 <%2>").arg(display, safeAddr);
}

QString formatAddressList(const QStringList& addrs) {
    QStringList out;
    out.reserve(addrs.size());
    for (const auto& a : addrs) out << sanitizeHeaderField(a);
    return out.join(QStringLiteral(", "));
}

QString rfc5322Date() {
    // Sun, 06 Nov 1994 08:49:37 GMT
    return QLocale::c().toString(
        QDateTime::currentDateTimeUtc(),
        QStringLiteral("ddd, dd MMM yyyy HH:mm:ss"))
        + QStringLiteral(" +0000");
}

// RFC 2045 §6.8: base64 in headers/bodies must wrap at 76 chars.
QByteArray wrapBase64(const QByteArray& raw) {
    const QByteArray b64 = raw.toBase64();
    QByteArray out;
    out.reserve(b64.size() + b64.size() / 76 * 2);
    constexpr int kWidth = 76;
    for (int i = 0; i < b64.size(); i += kWidth) {
        out.append(b64.mid(i, kWidth));
        out.append("\r\n");
    }
    return out;
}

QByteArray normalizeBodyCrlf(QString body) {
    body.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    body.replace(QChar('\n'), QStringLiteral("\r\n"));
    return body.toUtf8();
}

// Mime boundary derived from a UUID — guaranteed unique within a message.
// Prefixed with "fc--" so a casual reader can tell it came from us.
QByteArray newBoundary() {
    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return ("fc--" + uuid).toLatin1();
}

}  // namespace

QString MimeBuilder::newMessageId(const QString& fromAddr) {
    QString domain = QStringLiteral("firstcontact.local");
    const QString safeFrom = sanitizeHeaderField(fromAddr);
    const int at = safeFrom.indexOf('@');
    if (at > 0 && at + 1 < safeFrom.size()) {
        domain = safeFrom.mid(at + 1);
    }
    QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return QStringLiteral("<%1@%2>").arg(uuid, domain);
}

QByteArray MimeBuilder::build(const OutgoingMessage& msg) {
    QString headers;
    headers += QStringLiteral("From: %1\r\n").arg(
        formatAddress(msg.fromAddr, msg.fromName));
    headers += QStringLiteral("To: %1\r\n").arg(formatAddressList(msg.to));
    if (!msg.cc.isEmpty())
        headers += QStringLiteral("Cc: %1\r\n").arg(formatAddressList(msg.cc));
    if (!msg.bcc.isEmpty())
        headers += QStringLiteral("Bcc: %1\r\n").arg(formatAddressList(msg.bcc));
    headers += QStringLiteral("Subject: %1\r\n").arg(
        encodedWord(sanitizeHeaderField(msg.subject)));
    headers += QStringLiteral("Date: %1\r\n").arg(rfc5322Date());
    headers += QStringLiteral("Message-ID: %1\r\n").arg(newMessageId(msg.fromAddr));
    headers += QStringLiteral("MIME-Version: 1.0\r\n");

    const bool multipart = !msg.bodyHtml.isEmpty();
    QByteArray boundary;
    if (multipart) {
        boundary = newBoundary();
        headers += QStringLiteral(
            "Content-Type: multipart/alternative; boundary=\"%1\"\r\n")
            .arg(QString::fromLatin1(boundary));
    } else {
        headers += QStringLiteral("Content-Type: text/plain; charset=UTF-8\r\n");
        headers += QStringLiteral("Content-Transfer-Encoding: 8bit\r\n");
    }

    if (!msg.rfc822InReplyTo.isEmpty()) {
        headers += QStringLiteral("In-Reply-To: %1\r\n").arg(
            sanitizeHeaderField(msg.rfc822InReplyTo));
    }
    if (!msg.rfc822References.isEmpty()) {
        QStringList safeRefs;
        safeRefs.reserve(msg.rfc822References.size());
        for (const auto& r : msg.rfc822References) safeRefs << sanitizeHeaderField(r);
        headers += QStringLiteral("References: %1\r\n").arg(
            safeRefs.join(QChar(' ')));
    } else if (!msg.rfc822InReplyTo.isEmpty()) {
        headers += QStringLiteral("References: %1\r\n").arg(
            sanitizeHeaderField(msg.rfc822InReplyTo));
    }

    QByteArray out;
    out.append(headers.toUtf8());
    out.append("\r\n");

    if (!multipart) {
        out.append(normalizeBodyCrlf(msg.bodyText));
        return out;
    }

    // RFC 2046 §5.1.4: ordering matters in multipart/alternative — most
    // basic representation FIRST, richest LAST. Conformant clients render
    // the LAST part they understand. Plain text first, HTML last.
    auto appendPart = [&](const QByteArray& mimeType,
                           const QString& body) {
        out.append("--");
        out.append(boundary);
        out.append("\r\n");
        out.append("Content-Type: ");
        out.append(mimeType);
        out.append("; charset=UTF-8\r\n");
        out.append("Content-Transfer-Encoding: base64\r\n");
        out.append("\r\n");
        out.append(wrapBase64(normalizeBodyCrlf(body)));
    };

    // RFC 2046 §5.1.1 also recommends a leading "preamble" before the
    // first boundary marker for clients that don't understand
    // multipart at all. Keep it short and explanatory.
    out.append("This is a multi-part message in MIME format.\r\n");

    appendPart("text/plain", msg.bodyText);
    appendPart("text/html",  msg.bodyHtml);

    // Closing boundary.
    out.append("--");
    out.append(boundary);
    out.append("--\r\n");
    return out;
}

}  // namespace fc::util
