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
                             const QString& format,
                             std::function<void(fc::Message, ApiError)> cb) {
    QUrl url = base(QStringLiteral("/messages/") + id);
    QUrlQuery q;
    const QString fmt = format.isEmpty() ? QStringLiteral("full") : format;
    q.addQueryItem(QStringLiteral("format"), fmt);
    url.setQuery(q);

    rest_->send(RestClient::Verb::Get, url, {}, {},
        [cb = std::move(cb)](QByteArray body, ApiError err) {
            if (err) { cb({}, err); return; }
            cb(MessageParser::parse(QJsonDocument::fromJson(body).object()), {});
        });
}

void GmailClient::getMessage(const QString& id,
                             std::function<void(fc::Message, ApiError)> cb) {
    getMessage(id, QStringLiteral("full"), std::move(cb));
}

void GmailClient::batchGetMessages(
        const QStringList& ids,
        const QString& format,
        std::function<void(std::vector<BatchMessageResult>, ApiError)> cb) {
    if (ids.isEmpty()) {
        cb({}, {});
        return;
    }
    const QString fmt = format.isEmpty() ? QStringLiteral("metadata") : format;

    std::vector<RestClient::BatchSubRequest> requests;
    requests.reserve(ids.size());
    for (const QString& id : ids) {
        RestClient::BatchSubRequest r;
        r.verb = RestClient::Verb::Get;
        // The path is relative to https://gmail.googleapis.com — include
        // /gmail/v1/users/me explicitly because the batch endpoint itself
        // is rooted at /batch (no implicit /gmail/v1 prefix).
        r.path = (QStringLiteral("/gmail/v1/users/me/messages/")
                  + id
                  + QStringLiteral("?format=") + fmt).toUtf8();
        requests.push_back(std::move(r));
    }

    rest_->sendBatch(std::move(requests),
        [ids, cb = std::move(cb)]
        (std::vector<RestClient::BatchSubResult> parts, ApiError err) {
            if (err) { cb({}, err); return; }
            std::vector<BatchMessageResult> out;
            out.reserve(parts.size());
            for (size_t i = 0; i < parts.size(); ++i) {
                BatchMessageResult br;
                const auto& p = parts[i];
                if (p.status >= 200 && p.status < 300 && !p.body.isEmpty()) {
                    br.msg = MessageParser::parse(
                        QJsonDocument::fromJson(p.body).object());
                } else if (p.status == 0) {
                    br.err = ApiError{ApiErrorKind::Parse, 0,
                                      QStringLiteral("batch: missing sub-response"),
                                      {}};
                } else {
                    br.err = ApiError{};
                    br.err.httpStatus = p.status;
                    if (p.status == 429) br.err.kind = ApiErrorKind::RateLimited;
                    else if (p.status == 404) br.err.kind = ApiErrorKind::NotFound;
                    else if (p.status >= 500) br.err.kind = ApiErrorKind::Server;
                    else if (p.status >= 400) br.err.kind = ApiErrorKind::BadRequest;
                    else br.err.kind = ApiErrorKind::Other;
                }
                out.push_back(std::move(br));
            }
            // If every sub-response is empty/missing, surface a single
            // parse error so the caller doesn't silently get a vector
            // of empties.
            bool anyOk = false;
            for (const auto& r : out) {
                if (!r.msg.id.isEmpty()) { anyOk = true; break; }
            }
            if (!anyOk && !out.empty()) {
                cb(std::move(out),
                   ApiError{ApiErrorKind::Parse, 0,
                            QStringLiteral("batch: all sub-responses empty"),
                            {}});
                return;
            }
            cb(std::move(out), {});
        });
}

void GmailClient::getAttachment(const QString& messageId,
                                const QString& attachmentId,
                                std::function<void(QByteArray, ApiError)> cb) {
    const QUrl url = base(QStringLiteral("/messages/") + messageId
                          + QStringLiteral("/attachments/") + attachmentId);
    rest_->send(RestClient::Verb::Get, url, {}, {},
        [cb = std::move(cb)](QByteArray body, ApiError err) {
            if (err) { cb({}, err); return; }
            // The response is { size, data }; data is base64url-encoded
            // (Gmail uses URL-safe alphabet WITHOUT padding). Decode here so
            // every caller doesn't reinvent the decode.
            const auto o = QJsonDocument::fromJson(body).object();
            const QByteArray b64 =
                o.value(QStringLiteral("data")).toString().toLatin1();
            cb(QByteArray::fromBase64(
                   b64, QByteArray::Base64UrlEncoding
                            | QByteArray::OmitTrailingEquals),
               {});
        });
}

void GmailClient::getProfile(std::function<void(Profile, ApiError)> cb) {
    rest_->send(RestClient::Verb::Get, base(QStringLiteral("/profile")), {}, {},
        [cb = std::move(cb)](QByteArray body, ApiError err) {
            if (err) { cb({}, err); return; }
            const auto o = QJsonDocument::fromJson(body).object();
            Profile p;
            p.emailAddress = o.value(QStringLiteral("emailAddress")).toString();
            // Gmail returns historyId as a JSON string (the value is a
            // uint64, too large for IEEE-754 doubles to hold without
            // precision loss). The previous code went through
            // toDouble() — which returns 0 for non-number values —
            // and stored "0" as the historyId. listHistory then 400-d
            // on "0", clearing the slot and re-entering initial sync
            // on every tick, while messages from each fetch leaked
            // through with empty accountId stamps.
            p.historyId = o.value(QStringLiteral("historyId")).toString();
            cb(p, {});
        });
}

namespace {

GmailClient::Label labelFromJson(const QJsonObject& o) {
    GmailClient::Label l;
    l.id   = o.value(QStringLiteral("id")).toString();
    l.name = o.value(QStringLiteral("name")).toString();
    l.type = o.value(QStringLiteral("type")).toString();
    const auto color = o.value(QStringLiteral("color")).toObject();
    l.colorBg = color.value(QStringLiteral("backgroundColor")).toString();
    l.colorFg = color.value(QStringLiteral("textColor")).toString();
    return l;
}

}  // namespace

void GmailClient::listLabels(std::function<void(std::vector<Label>, ApiError)> cb) {
    rest_->send(RestClient::Verb::Get, base(QStringLiteral("/labels")), {}, {},
        [cb = std::move(cb)](QByteArray body, ApiError err) {
            if (err) { cb({}, err); return; }
            std::vector<Label> out;
            for (const auto v : QJsonDocument::fromJson(body)
                                    .object().value(QStringLiteral("labels")).toArray()) {
                out.push_back(labelFromJson(v.toObject()));
            }
            cb(std::move(out), {});
        });
}

void GmailClient::createLabel(const QString& name,
                              std::function<void(Label, ApiError)> cb) {
    QJsonObject body{
        {QStringLiteral("name"), name},
        {QStringLiteral("messageListVisibility"), QStringLiteral("show")},
        {QStringLiteral("labelListVisibility"),   QStringLiteral("labelShow")},
    };
    rest_->send(RestClient::Verb::Post, base(QStringLiteral("/labels")),
        QJsonDocument(body).toJson(QJsonDocument::Compact),
        "application/json",
        [cb = std::move(cb)](QByteArray data, ApiError err) {
            if (err) { cb({}, err); return; }
            cb(labelFromJson(QJsonDocument::fromJson(data).object()), {});
        });
}

void GmailClient::updateLabel(const QString& id, const QString& newName,
                              std::function<void(Label, ApiError)> cb) {
    QJsonObject body{ {QStringLiteral("name"), newName} };
    rest_->send(RestClient::Verb::Patch,
        base(QStringLiteral("/labels/") + id),
        QJsonDocument(body).toJson(QJsonDocument::Compact),
        "application/json",
        [cb = std::move(cb)](QByteArray data, ApiError err) {
            if (err) { cb({}, err); return; }
            cb(labelFromJson(QJsonDocument::fromJson(data).object()), {});
        });
}

void GmailClient::deleteLabel(const QString& id,
                              std::function<void(ApiError)> cb) {
    rest_->send(RestClient::Verb::Delete,
        base(QStringLiteral("/labels/") + id), {}, {},
        [cb = std::move(cb)](QByteArray, ApiError err) { cb(err); });
}

void GmailClient::modifyMessage(const QString& messageId,
                                const QStringList& addLabels,
                                const QStringList& removeLabels,
                                std::function<void(ApiError)> cb) {
    QJsonArray adds;     for (const auto& s : addLabels)    adds.append(s);
    QJsonArray removes;  for (const auto& s : removeLabels) removes.append(s);
    QJsonObject body{
        {QStringLiteral("addLabelIds"),    adds},
        {QStringLiteral("removeLabelIds"), removes},
    };
    rest_->send(RestClient::Verb::Post,
        base(QStringLiteral("/messages/") + messageId + QStringLiteral("/modify")),
        QJsonDocument(body).toJson(QJsonDocument::Compact),
        "application/json",
        [cb = std::move(cb)](QByteArray, ApiError err) { cb(err); });
}

void GmailClient::listHistory(const QString& startHistoryId,
                              const QString& pageToken,
                              std::function<void(HistoryPage, ApiError)> cb) {
    QUrl url = base(QStringLiteral("/history"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("startHistoryId"), startHistoryId);
    q.addQueryItem(QStringLiteral("historyTypes"),   QStringLiteral("messageAdded"));
    q.addQueryItem(QStringLiteral("historyTypes"),   QStringLiteral("messageDeleted"));
    q.addQueryItem(QStringLiteral("historyTypes"),   QStringLiteral("labelAdded"));
    q.addQueryItem(QStringLiteral("historyTypes"),   QStringLiteral("labelRemoved"));
    if (!pageToken.isEmpty()) q.addQueryItem(QStringLiteral("pageToken"), pageToken);
    url.setQuery(q);

    rest_->send(RestClient::Verb::Get, url, {}, {},
        [cb = std::move(cb)](QByteArray body, ApiError err) {
            HistoryPage p;
            if (err.kind == ApiErrorKind::NotFound) {
                p.historyTooOld = true;
                cb(p, {});
                return;
            }
            if (err) { cb({}, err); return; }
            const auto o = QJsonDocument::fromJson(body).object();
            // Gmail wire format: historyId / history.id are JSON
            // strings (the values are uint64). Reading them as
            // doubles silently returns 0 — see the matching note in
            // getProfile.
            p.historyId     = o.value(QStringLiteral("historyId")).toString();
            p.nextPageToken = o.value(QStringLiteral("nextPageToken")).toString();
            for (const auto v : o.value(QStringLiteral("history")).toArray()) {
                const auto h = v.toObject();
                HistoryEntry e;
                e.id = h.value(QStringLiteral("id")).toString();
                for (const auto m : h.value(QStringLiteral("messagesAdded")).toArray()) {
                    e.messagesAdded
                        << m.toObject().value(QStringLiteral("message"))
                                       .toObject().value(QStringLiteral("id")).toString();
                }
                for (const auto m : h.value(QStringLiteral("messagesDeleted")).toArray()) {
                    e.messagesDeleted
                        << m.toObject().value(QStringLiteral("message"))
                                       .toObject().value(QStringLiteral("id")).toString();
                }
                auto readLabelDeltas =
                    [](const QJsonArray& arr) {
                        std::vector<HistoryEntry::LabelDelta> out;
                        out.reserve(static_cast<size_t>(arr.size()));
                        for (const auto deltaValue : arr) {
                            const auto deltaObject = deltaValue.toObject();
                            HistoryEntry::LabelDelta d;
                            d.messageId =
                                deltaObject.value(QStringLiteral("message"))
                                .toObject()
                                .value(QStringLiteral("id")).toString();
                            for (const auto label
                                 : deltaObject.value(QStringLiteral("labelIds"))
                                   .toArray()) {
                                d.labelIds << label.toString();
                            }
                            if (!d.messageId.isEmpty() && !d.labelIds.isEmpty()) {
                                out.push_back(std::move(d));
                            }
                        }
                        return out;
                    };
                e.labelsAdded = readLabelDeltas(
                    h.value(QStringLiteral("labelsAdded")).toArray());
                e.labelsRemoved = readLabelDeltas(
                    h.value(QStringLiteral("labelsRemoved")).toArray());
                p.entries.push_back(std::move(e));
            }
            cb(p, {});
        });
}

void GmailClient::sendRaw(const QByteArray& rfc5322,
                          const QString& threadId,
                          std::function<void(QString, ApiError)> cb) {
    QJsonObject body{
        {QStringLiteral("raw"),
         QString::fromLatin1(rfc5322.toBase64(
             QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))},
    };
    if (!threadId.isEmpty()) {
        body.insert(QStringLiteral("threadId"), threadId);
    }
    rest_->send(RestClient::Verb::Post, base(QStringLiteral("/messages/send")),
        QJsonDocument(body).toJson(QJsonDocument::Compact),
        "application/json",
        [cb = std::move(cb)](QByteArray data, ApiError err) {
            if (err) { cb({}, err); return; }
            cb(QJsonDocument::fromJson(data).object()
                  .value(QStringLiteral("id")).toString(), {});
        });
}

void GmailClient::createDraft(const QByteArray& rfc5322,
                              const QString& threadId,
                              std::function<void(QString, ApiError)> cb) {
    QJsonObject message{
        {QStringLiteral("raw"),
         QString::fromLatin1(rfc5322.toBase64(
             QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))},
    };
    if (!threadId.isEmpty()) {
        message.insert(QStringLiteral("threadId"), threadId);
    }
    QJsonObject body{ {QStringLiteral("message"), message} };
    rest_->send(RestClient::Verb::Post, base(QStringLiteral("/drafts")),
        QJsonDocument(body).toJson(QJsonDocument::Compact),
        "application/json",
        [cb = std::move(cb)](QByteArray data, ApiError err) {
            if (err) { cb({}, err); return; }
            cb(QJsonDocument::fromJson(data).object()
                  .value(QStringLiteral("id")).toString(), {});
        });
}

void GmailClient::deleteDraft(const QString& draftId,
                              std::function<void(ApiError)> cb) {
    rest_->send(RestClient::Verb::Delete,
        base(QStringLiteral("/drafts/") + draftId), {}, {},
        [cb = std::move(cb)](QByteArray, ApiError err) { cb(err); });
}

}  // namespace fc::api
