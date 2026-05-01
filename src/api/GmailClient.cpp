#include "GmailClient.h"

#include "MessageParser.h"
#include "RestClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

namespace fc::api {

namespace {

QUrl base(const QString& path) {
    return QUrl(QStringLiteral("https://gmail.googleapis.com/gmail/v1/users/me") + path);
}

}  // namespace

GmailClient::GmailClient(RestClient* rest, QObject* parent)
    : QObject(parent), rest_(rest) {}

void GmailClient::listMessages(const QString& labelId,
                               const QString& q,
                               const QString& pageToken,
                               int maxResults,
                               std::function<void(ListPage, ApiError)> cb) {
    QUrl url = base(QStringLiteral("/messages"));
    QUrlQuery query;
    if (!labelId.isEmpty())   query.addQueryItem(QStringLiteral("labelIds"),  labelId);
    if (!q.isEmpty())         query.addQueryItem(QStringLiteral("q"),         q);
    if (!pageToken.isEmpty()) query.addQueryItem(QStringLiteral("pageToken"), pageToken);
    if (maxResults > 0)       query.addQueryItem(QStringLiteral("maxResults"),
                                                 QString::number(maxResults));
    url.setQuery(query);

    rest_->send(RestClient::Verb::Get, url, {}, {},
        [cb = std::move(cb)](QByteArray body, ApiError err) {
            if (err) { cb({}, err); return; }
            const auto o = QJsonDocument::fromJson(body).object();
            ListPage p;
            for (const auto v : o.value(QStringLiteral("messages")).toArray()) {
                p.ids << v.toObject().value(QStringLiteral("id")).toString();
            }
            p.nextPageToken      = o.value(QStringLiteral("nextPageToken")).toString();
            p.resultSizeEstimate = static_cast<qint64>(
                o.value(QStringLiteral("resultSizeEstimate")).toDouble());
            cb(p, {});
        });
}

void GmailClient::getMessage(const QString& id,
                             std::function<void(fc::Message, ApiError)> cb) {
    QUrl url = base(QStringLiteral("/messages/") + id);
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("full"));
    url.setQuery(q);

    rest_->send(RestClient::Verb::Get, url, {}, {},
        [cb = std::move(cb)](QByteArray body, ApiError err) {
            if (err) { cb({}, err); return; }
            cb(MessageParser::parse(QJsonDocument::fromJson(body).object()), {});
        });
}

void GmailClient::getProfile(std::function<void(Profile, ApiError)> cb) {
    rest_->send(RestClient::Verb::Get, base(QStringLiteral("/profile")), {}, {},
        [cb = std::move(cb)](QByteArray body, ApiError err) {
            if (err) { cb({}, err); return; }
            const auto o = QJsonDocument::fromJson(body).object();
            Profile p;
            p.emailAddress = o.value(QStringLiteral("emailAddress")).toString();
            p.historyId    = QString::number(static_cast<qint64>(
                                o.value(QStringLiteral("historyId")).toDouble()));
            cb(p, {});
        });
}

}  // namespace fc::api
