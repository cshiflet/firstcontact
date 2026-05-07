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
//   - text/html (full)        -> served via LocalHtmlServer + the system
//                                browser ("Open in browser" button)
class ReaderPane : public QWidget {
    Q_OBJECT
public:
    explicit ReaderPane(QWidget* parent = nullptr);

    void showLoading();
    void showMessage(const fc::Message& m);
    // Renders a stack of message cards for an entire thread. When
    // `focusedId` is empty (or doesn't match any message in the
    // thread), the latest message is expanded and the rest collapsed
    // — Gmail-web's default. When `focusedId` matches a message in
    // the thread, THAT card is expanded instead and scrolled into
    // view, so a click on a child row in the conversation-view
    // threadlist lands the user on the right message.
    void showThread(const std::vector<fc::Message>& messages,
                     const QString& focusedId = {});
    void showEmpty(const QString& reason = {});

signals:
    // Fired on left-click of an attachment chip. Slot owner writes the
    // bytes to a per-session temp directory and hands the path to
    // QDesktopServices::openUrl — the file is NOT recorded as a permanent
    // download. The temp directory is wiped on app exit.
    void openAttachmentRequested(const QString& messageId,
                                  const QString& attachmentId,
                                  const QString& filename);

    // Fired on right-click → "Save as…". Slot owner shows a file picker
    // (initial directory = Preferences::attachmentDir()) and writes the
    // bytes to whatever path the user confirms.
    void saveAsAttachmentRequested(const QString& messageId,
                                    const QString& attachmentId,
                                    const QString& filename);

    // Fired when the user activates the "Download all" button on a card
    // with two or more attachments. Slot owner picks a target folder
    // (a one-shot QFileDialog seeded with attachmentDir()) and writes
    // every file there with collision-safe naming.
    void downloadAllRequested(const QString& messageId);

private:
    void clearStack();
    QWidget* buildMessageCard(const fc::Message& m, bool initiallyExpanded);

    QScrollArea* scroll_;
    QWidget*     content_;
    QVBoxLayout* contentLayout_;
};

}  // namespace fc::ui
