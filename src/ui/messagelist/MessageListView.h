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
    // Fired when the user clicks the star area of a row. We DON'T also
    // emit messageActivated for that click — toggling a star on a
    // non-selected row shouldn't open the message.
    void starToggled(const QString& messageId);

protected:
    void mousePressEvent(QMouseEvent* e) override;

private slots:
    void onActivated(const QModelIndex& idx);
};

}  // namespace fc::ui
