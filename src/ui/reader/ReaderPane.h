#pragma once

#include "models/Message.h"

#include <QWidget>

#include <vector>

class QTextBrowser;
class QLabel;
class QVBoxLayout;
class QScrollArea;

namespace fc::ui {

// Right-hand pane. Three rendering modes:
//   - showLoading()        — single placeholder
//   - showMessage(m)       — single message (used by search results)
//   - showThread(messages) — Gmail-web-style stack: oldest first, latest
//                            expanded by default, older messages collapsed
//                            into header-only cards that expand on click.
//
// Body rendering tiers:
//   - text/plain              -> linkified plain text in QTextBrowser
//   - text/html (sanitized)   -> HtmlSanitizer rich tier into QTextBrowser
//   - text/html (full WebEngine) -> Phase-3 plugin via HtmlRenderHost
class ReaderPane : public QWidget {
    Q_OBJECT
public:
    explicit ReaderPane(QWidget* parent = nullptr);

    void showLoading();
    void showMessage(const fc::Message& m);
    void showThread(const std::vector<fc::Message>& messages);
    void showEmpty(const QString& reason = {});

signals:
    // Fired when the user activates one of the attachment chips. The slot
    // owner (MainWindow) bridges to GmailClient::getAttachment + writes
    // the bytes to disk; ReaderPane stays UI-only and ignorant of the
    // network layer.
    //
    // forceSaveAs=true means the user picked "Save as…" from the right-
    // click menu — the slot must show a file picker regardless of the
    // global "always ask" preference. forceSaveAs=false leaves the
    // path-resolution policy up to the slot owner (which checks the pref).
    void downloadAttachmentRequested(const QString& messageId,
                                      const QString& attachmentId,
                                      const QString& filename,
                                      bool forceSaveAs);

    // Fired when the user activates the "Download all" button on a card
    // with two or more attachments. Slot owner iterates the message's
    // attachments and dispatches a save chain — picking a target folder
    // once if the always-ask pref is on, then writing every file under
    // that folder with collision-safe naming.
    void downloadAllRequested(const QString& messageId);

private:
    void clearStack();
    QWidget* buildMessageCard(const fc::Message& m, bool initiallyExpanded);

    QScrollArea* scroll_;
    QWidget*     content_;
    QVBoxLayout* contentLayout_;
};

}  // namespace fc::ui
