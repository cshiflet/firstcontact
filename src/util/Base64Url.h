#pragma once

#include <QByteArray>

namespace fc::util {

// Gmail uses base64url (RFC 4648 §5) without padding for the `raw` field
// of messages.send and for the OAuth PKCE code_challenge.
QByteArray base64UrlEncode(const QByteArray& bytes);
QByteArray base64UrlDecode(const QByteArray& text);

}  // namespace fc::util
