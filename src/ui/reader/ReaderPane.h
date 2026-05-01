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

private:
    void clearStack();
    QWidget* buildMessageCard(const fc::Message& m, bool initiallyExpanded);

    QScrollArea* scroll_;
    QWidget*     content_;
    QVBoxLayout* contentLayout_;
};

}  // namespace fc::ui
