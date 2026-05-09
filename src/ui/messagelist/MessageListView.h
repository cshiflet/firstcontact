#pragma once

#include <QListView>
#include <QString>

namespace fc::ui {

// Wraps QListView with FirstContact-specific defaults: row height, single-
// selection by default, signal for "open this message id" on activation.
class MessageListView : public QListView {
    Q_OBJECT
public:
    explicit MessageListView(QWidget* parent = nullptr);

    // Customises the centred placeholder shown when the model has zero
    // rows. Defaults to "No messages" + a hint. Owners that know the
    // current source (e.g. "search returned nothing", "label is empty")
    // can pass a more specific message.
    void setEmptyText(const QString& title, const QString& subtitle = {});

signals:
    void messageActivated(const QString& messageId, int row);
    // Fired when the user clicks the star area of a row. We DON'T also
    // emit messageActivated for that click — toggling a star on a
    // non-selected row shouldn't open the message.
    void starToggled(const QString& messageId);

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void paintEvent(QPaintEvent* e) override;

private slots:
    void onActivated(const QModelIndex& idx);

private:
    QString emptyTitle_;
    QString emptySubtitle_;
};

}  // namespace fc::ui
