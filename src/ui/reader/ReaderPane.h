#pragma once

#include "models/Message.h"

#include <QWidget>

class QTextBrowser;
class QLabel;

namespace fc::ui {

// Right-hand pane: header summary on top, body below. Phase-1 renders the
// plain-text part only; Phase-3 adds the sanitized-HTML tier and the lazy
// QtWebEngine "Show full HTML" tier.
class ReaderPane : public QWidget {
    Q_OBJECT
public:
    explicit ReaderPane(QWidget* parent = nullptr);

    void showLoading();
    void showMessage(const fc::Message& m);
    void showEmpty(const QString& reason = {});

private:
    QLabel*       headerLabel_;
    QTextBrowser* body_;
};

}  // namespace fc::ui
