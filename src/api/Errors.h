#pragma once

#include <QString>

namespace fc::api {

enum class ApiErrorKind {
    None,
    Network,        // QNetworkReply transport-level failure
    Auth,           // 401 after refresh
    RateLimited,    // 429 / userRateLimitExceeded
    NotFound,       // 404 (incl. historyId-not-found)
    BadRequest,     // 400
    Server,         // 5xx
    Cancelled,
    Parse,
    Other,
};

struct ApiError {
    ApiErrorKind kind = ApiErrorKind::None;
    int httpStatus = 0;
    QString message;
    QString gmailReason;   // populated from JSON `error.errors[0].reason` when present

    bool isOk() const { return kind == ApiErrorKind::None; }
    explicit operator bool() const { return kind != ApiErrorKind::None; }
};

}  // namespace fc::api
