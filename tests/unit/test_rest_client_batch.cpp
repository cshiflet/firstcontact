// Exercises RestClient::parseBatchResponse on a synthetic Gmail /batch
// multipart/mixed envelope. The parser has to:
//   - split parts on "--<boundary>",
//   - map each part back to its sub-request index via Content-ID,
//   - extract the inner HTTP status line + body,
//   - tolerate per-sub-request errors (e.g. one 404 alongside several 200s).
//
// The parser is exercised in isolation here — no network. The full
// send-batch path is covered by integration when SyncService starts
// using it.

#include "api/RestClient.h"

#include <QByteArray>
#include <QObject>
#include <QtTest>

using fc::api::RestClient;

namespace {

// Build a representative response body. The shape mirrors what Gmail
// returns: one application/http part per request, with response-item-N
// content ids that align by index.
QByteArray buildResponse(const QByteArray& boundary,
                          const std::vector<QPair<int, QByteArray>>& parts) {
    QByteArray out;
    for (size_t i = 0; i < parts.size(); ++i) {
        out += "--";
        out += boundary;
        out += "\r\n";
        out += "Content-Type: application/http\r\n";
        out += "Content-ID: <response-item-";
        out += QByteArray::number(qulonglong(i));
        out += ">\r\n\r\n";
        out += "HTTP/1.1 ";
        out += QByteArray::number(parts[i].first);
        out += " Test\r\n";
        out += "Content-Type: application/json; charset=UTF-8\r\n";
        out += "Content-Length: ";
        out += QByteArray::number(parts[i].second.size());
        out += "\r\n\r\n";
        out += parts[i].second;
        out += "\r\n";
    }
    out += "--";
    out += boundary;
    out += "--\r\n";
    return out;
}

}  // namespace

class TestRestClientBatch : public QObject {
    Q_OBJECT
private slots:
    // Bedrock happy path: two 200 sub-responses, parser surfaces both
    // bodies in order.
    void parsesAlignedSuccessResponses() {
        const QByteArray boundary = "batch_test_1234";
        const QByteArray a = R"({"id":"msg-a","threadId":"t-a"})";
        const QByteArray b = R"({"id":"msg-b","threadId":"t-b"})";
        const QByteArray body = buildResponse(boundary, {{200, a}, {200, b}});
        const auto results = RestClient::parseBatchResponse(body, boundary, 2);
        QCOMPARE(int(results.size()), 2);
        QCOMPARE(results[0].status, 200);
        QCOMPARE(results[1].status, 200);
        QCOMPARE(results[0].body, a);
        QCOMPARE(results[1].body, b);
    }

    // Per-sub-request error: one 404 alongside one 200 — both must
    // round-trip cleanly with their respective statuses.
    void surfacesPerPartStatuses() {
        const QByteArray boundary = "batch_test_err";
        const QByteArray okBody = R"({"id":"msg-ok"})";
        const QByteArray errBody = R"({"error":{"code":404,"message":"Not Found"}})";
        const QByteArray body = buildResponse(boundary,
            {{200, okBody}, {404, errBody}});
        const auto results = RestClient::parseBatchResponse(body, boundary, 2);
        QCOMPARE(int(results.size()), 2);
        QCOMPARE(results[0].status, 200);
        QCOMPARE(results[0].body, okBody);
        QCOMPARE(results[1].status, 404);
        QCOMPARE(results[1].body, errBody);
    }

    // Content-ID ordering: even if the server returns parts in a
    // different order than requests went out, the parser routes by
    // the response-item-N id so each result lands at its correct
    // index.
    void routesByContentIdNotPositional() {
        const QByteArray boundary = "batch_test_ooo";
        // Build a body with parts in reversed order by hand.
        QByteArray body;
        const QByteArray bodyB = R"({"id":"msg-b"})";
        const QByteArray bodyA = R"({"id":"msg-a"})";
        body += "--"; body += boundary; body += "\r\n";
        body += "Content-Type: application/http\r\n";
        body += "Content-ID: <response-item-1>\r\n\r\n";
        body += "HTTP/1.1 200 OK\r\n\r\n";
        body += bodyB;
        body += "\r\n";
        body += "--"; body += boundary; body += "\r\n";
        body += "Content-Type: application/http\r\n";
        body += "Content-ID: <response-item-0>\r\n\r\n";
        body += "HTTP/1.1 200 OK\r\n\r\n";
        body += bodyA;
        body += "\r\n";
        body += "--"; body += boundary; body += "--\r\n";

        const auto results = RestClient::parseBatchResponse(body, boundary, 2);
        QCOMPARE(int(results.size()), 2);
        QCOMPARE(results[0].body, bodyA);
        QCOMPARE(results[1].body, bodyB);
    }

    // Missing part: requested 3, server only returned 2. The third
    // result slot must remain status=0 + empty body so callers can
    // detect the gap and re-request.
    void leavesMissingSlotsEmpty() {
        const QByteArray boundary = "batch_test_gap";
        const QByteArray a = R"({"id":"msg-a"})";
        const QByteArray b = R"({"id":"msg-b"})";
        const QByteArray body = buildResponse(boundary, {{200, a}, {200, b}});
        const auto results = RestClient::parseBatchResponse(body, boundary, 3);
        QCOMPARE(int(results.size()), 3);
        QCOMPARE(results[0].status, 200);
        QCOMPARE(results[1].status, 200);
        QCOMPARE(results[2].status, 0);
        QVERIFY(results[2].body.isEmpty());
    }

    // Regression for the silent failure in 57b3f8c: Gmail's actual
    // /batch responses prefix the body with a leading CRLF before
    // the first "--<boundary>\r\n" marker. The boundary extractor
    // must skip that leading whitespace, not reject the whole
    // response as malformed.
    void extractsBoundaryDespiteLeadingCrlf() {
        const QByteArray boundary = "batch_lead_crlf";
        QByteArray body = "\r\n";   // leading CRLF Gmail actually emits
        body += "--"; body += boundary; body += "\r\n";
        body += "Content-Type: application/http\r\n";
        body += "Content-ID: <response-item-0>\r\n\r\n";
        body += "HTTP/1.1 200 OK\r\n\r\n";
        body += "ok\r\n";
        body += "--"; body += boundary; body += "--\r\n";

        QCOMPARE(RestClient::extractBatchBoundary(body), boundary);
    }

    // Defensive: an empty / non-multipart response (e.g. Gmail
    // returned a JSON error envelope) must yield an empty
    // boundary so the caller's fallback path kicks in instead of
    // misinterpreting random JSON as multipart.
    void extractsEmptyBoundaryFromGarbage() {
        QCOMPARE(RestClient::extractBatchBoundary(QByteArray()), QByteArray());
        QCOMPARE(RestClient::extractBatchBoundary("{\"error\": \"bad\"}"),
                 QByteArray());
        // Only "--" with no boundary chars after, no CRLF terminator.
        QCOMPARE(RestClient::extractBatchBoundary("--"), QByteArray());
    }

    // Empty inner body: a 204 No Content sub-response should parse as
    // status=204 + empty body without crashing the parser.
    void parses204NoContent() {
        const QByteArray boundary = "batch_test_204";
        QByteArray body;
        body += "--"; body += boundary; body += "\r\n";
        body += "Content-Type: application/http\r\n";
        body += "Content-ID: <response-item-0>\r\n\r\n";
        body += "HTTP/1.1 204 No Content\r\n";
        body += "Content-Length: 0\r\n\r\n";
        body += "\r\n";
        body += "--"; body += boundary; body += "--\r\n";

        const auto results = RestClient::parseBatchResponse(body, boundary, 1);
        QCOMPARE(int(results.size()), 1);
        QCOMPARE(results[0].status, 204);
        QVERIFY(results[0].body.isEmpty());
    }
};

QTEST_APPLESS_MAIN(TestRestClientBatch)
#include "test_rest_client_batch.moc"
