#pragma once

#include <QIcon>
#include <QPixmap>
#include <QTimer>
#include <QToolButton>

namespace fc::ui {

// QToolButton that can rotate its icon in-place. Used for the toolbar
// refresh button: rotates while a sync is in flight, restores the
// static icon when sync ends.
//
// Implementation: stays a normal QToolButton most of the time. When
// setSpinning(true) is called, a 30 fps QTimer drives setIcon() with
// a rotated copy of the base pixmap. Cheap (one QPixmap transform
// per frame at the toolbar's icon size) and survives the existing
// theme/repaint plumbing because we go through the standard setIcon
// path.
class SpinningToolButton : public QToolButton {
    Q_OBJECT
public:
    explicit SpinningToolButton(QWidget* parent = nullptr);

    // Records the icon and pre-renders a base pixmap at the current
    // iconSize() for the rotation source. Call again after
    // IconLoader::themed re-tints on theme change so the spinning
    // icon picks up the new colour.
    void setBaseIcon(const QIcon& icon);

    void setSpinning(bool on);
    bool isSpinning() const { return spinning_; }

private slots:
    void onTick();

private:
    QIcon   baseIcon_;
    QPixmap basePixmap_;
    QTimer  timer_;
    qreal   angle_ = 0;
    bool    spinning_ = false;
};

}  // namespace fc::ui
