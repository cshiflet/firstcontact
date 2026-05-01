#pragma once

#include <QListView>

namespace fc::ui {

// Wraps QListView with FirstContact-specific defaults: row height, single-
// selection by default, signal for "open this message id" on activation.
class MessageListView : public QListView {
    Q_OBJECT
public:
    explicit MessageListView(QWidget* parent = nullptr);

signals:
    void messageActivated(const QString& messageId, int row);

private slots:
    void onActivated(const QModelIndex& idx);
};

}  // namespace fc::ui
