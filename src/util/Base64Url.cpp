#include "Base64Url.h"

namespace fc::util {

QByteArray base64UrlEncode(const QByteArray& bytes) {
    return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QByteArray base64UrlDecode(const QByteArray& text) {
    return QByteArray::fromBase64(text, QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

}  // namespace fc::util
