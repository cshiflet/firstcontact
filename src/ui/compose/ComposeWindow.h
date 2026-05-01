#pragma once

#include "models/Message.h"
#include "util/MimeBuilder.h"

#include <QWidget>

class QLineEdit;
class QTextEdit;
class QLabel;

namespace fc::ui {

// Top-level compose window. Phase-3: not embedded — multi-compose is the
// default since the window owns its own state. The Send button hands a
// fully-built RFC 5322 blob up via the `composeReady` signal; the caller
// (MainWindow / OutboxWorker) decides how to deliver it.
class ComposeWindow : public QWidget {
    Q_OBJECT
public:
    enum class Mode { New, Reply, ReplyAll, Forward };

    ComposeWindow(const QString& fromAddr,
                  const QString& fromName,
                  QWidget* parent = nullptr);

    // Pre-fills To/Subject/References from a parent message.
    void prefillFrom(const fc::Message& parent, Mode mode);

signals:
    // Emitted when the user clicks Send. `outgoing` is fully populated and
    // ready to feed to MimeBuilder; threadId is set on reply/replyAll.
    void composeReady(const fc::util::OutgoingMessage& outgoing,
                      const QString& threadId);

private slots:
    void onSend();

private:
    QString    fromAddr_;
    QString    fromName_;
    QString    threadId_;

    QLineEdit* toEdit_;
    QLineEdit* ccEdit_;
    QLineEdit* subjectEdit_;
    QTextEdit* bodyEdit_;
    QLabel*    statusLabel_;

    QString    inReplyToHeader_;
    QStringList referencesHeader_;
};

}  // namespace fc::ui
