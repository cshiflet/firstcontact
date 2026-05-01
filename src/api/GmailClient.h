#pragma once

#include "Errors.h"
#include "models/Message.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace fc::api {

class RestClient;

// High-level façade over the Gmail REST API. Phase 1 surface is intentionally
// small: list message ids in a label, fetch a single message in full. Phase 2/3
// add labels.list, history.list, batch metadata, drafts.*, etc.
class GmailClient : public QObject {
    Q_OBJECT
public:
    explicit GmailClient(RestClient* rest, QObject* parent = nullptr);

    struct ListPage {
        QStringList ids;
        QString     nextPageToken;
        qint64      resultSizeEstimate = 0;
    };

    // GET /gmail/v1/users/me/messages?labelIds=…&q=…&pageToken=…
    void listMessages(const QString& labelId,
                      const QString& q,
                      const QString& pageToken,
                      int maxResults,
                      std::function<void(ListPage, ApiError)> cb);

    // GET /gmail/v1/users/me/messages/{id}?format=full
    void getMessage(const QString& id,
                    std::function<void(fc::Message, ApiError)> cb);

    // GET /gmail/v1/users/me/profile
    struct Profile {
        QString emailAddress;
        QString historyId;
    };
    void getProfile(std::function<void(Profile, ApiError)> cb);

private:
    RestClient* rest_;
};

}  // namespace fc::api
