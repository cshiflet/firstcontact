#pragma once

#include "models/Message.h"

class QJsonObject;

namespace fc::api {

// Parses a Gmail messages.get?format=full|metadata response into a fc::Message.
// Walks the MIME tree to extract:
//   - text/plain part into body_text,
//   - notes presence of any text/html part (lazy fetch tier 2),
//   - attachment list (filename, mime, size, attachmentId).
class MessageParser {
public:
    static fc::Message parse(const QJsonObject& gmailMessage);
};

}  // namespace fc::api
