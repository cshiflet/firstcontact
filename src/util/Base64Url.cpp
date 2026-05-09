#include "Base64Url.h"

#include <QDebug>

namespace fc::util {

QByteArray base64UrlEncode(const QByteArray& bytes) {
    return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QByteArray base64UrlDecode(const QByteArray& text) {
    // Use the modern overload that returns a status code so we can
    // distinguish "decoded fine, the result happens to be empty"
    // from "input was malformed". A silent failure here cascades
    // into garbled message bodies / attachment content with no log
    // trail to point at the cause.
    auto result = QByteArray::fromBase64Encoding(
        text, QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    if (!result) {
        qWarning("base64UrlDecode: malformed input (%lld bytes)",
                 static_cast<long long>(text.size()));
        return {};
    }
    return result.decoded;
}

}  // namespace fc::util
