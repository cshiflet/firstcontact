#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace fc::util {

struct OutgoingMessage {
    QString fromName;
    QString fromAddr;
    QStringList to;
    QStringList cc;
    QStringList bcc;
    QString subject;
    QString bodyText;
    QString inReplyToMessageId;     // Gmail message id for reply (used to lookup Message-ID)
    QString rfc822InReplyTo;        // Message-ID header value (with <…>) of the parent
    QStringList rfc822References;   // accumulated References thread chain
};

class MimeBuilder {
public:
    // Builds an RFC 5322 message ready for Gmail's messages.send `raw` field.
    // The result is uncoded; the caller (GmailClient::sendRaw) handles
    // base64url encoding.
    static QByteArray build(const OutgoingMessage& msg);

    // Generates a Message-ID with the form "<…uuid…@firstcontact.local>".
    static QString newMessageId(const QString& fromAddr);
};

}  // namespace fc::util
