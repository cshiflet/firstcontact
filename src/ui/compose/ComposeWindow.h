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

    // Pre-fills the entire window from an existing draft (for editing).
    void loadFromDraft(const QString& draftId,
                       const QString& threadId,
                       const QString& subject,
                       const QStringList& to,
                       const QStringList& cc,
                       const QString& body);

    // Returns the local draft id assigned on the first save (empty if not
    // saved yet).
    QString draftId() const;

signals:
    // Emitted when the user clicks Send. `outgoing` is fully populated and
    // ready to feed to MimeBuilder; threadId is set on reply/replyAll.
    void composeReady(const fc::util::OutgoingMessage& outgoing,
                      const QString& threadId);

    // Emitted when the user clicks Save Draft (or closes a dirty window after
    // confirming). MainWindow persists into DraftRepository and triggers
    // DraftSync to push to Gmail.
    void saveDraftRequested(const fc::util::OutgoingMessage& outgoing,
                            const QString& threadId,
                            const QString& existingDraftId);

private slots:
    void onSend();
    void onSaveDraft();

protected:
    void closeEvent(QCloseEvent* e) override;

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
    QString    draftId_;
    bool       dirty_ = false;
    bool       suppressClosePrompt_ = false;

    fc::util::OutgoingMessage currentMessage() const;
};

}  // namespace fc::ui
