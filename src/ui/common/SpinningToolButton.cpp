#include "SpinningToolButton.h"

#include <QTransform>

#include <cmath>

namespace fc::ui {

SpinningToolButton::SpinningToolButton(QWidget* parent) : QToolButton(parent) {
    timer_.setInterval(33);   // ~30 fps
    connect(&timer_, &QTimer::timeout, this, &SpinningToolButton::onTick);
}

void SpinningToolButton::setBaseIcon(const QIcon& icon) {
    baseIcon_ = icon;
    // Cache the rendered pixmap once so each tick reuses the same
    // source instead of re-rasterising the SVG. iconSize() reflects
    // whatever the parent toolbar set; if the toolbar's icon size
    // changes later, just call setBaseIcon again.
    basePixmap_ = icon.pixmap(iconSize());
    if (!spinning_) setIcon(icon);
}

void SpinningToolButton::setSpinning(bool on) {
    if (spinning_ == on) return;
    spinning_ = on;
    if (on) {
        if (basePixmap_.isNull()) basePixmap_ = baseIcon_.pixmap(iconSize());
        timer_.start();
    } else {
        timer_.stop();
        angle_ = 0;
        // Restore the un-rotated icon. setIcon with the original QIcon
        // (not a QPixmap) is important — QIcon may carry per-DPR
        // multi-resolution pixmaps that the rotated single-pixmap
        // path would otherwise discard.
        setIcon(baseIcon_);
        update();
    }
}

void SpinningToolButton::onTick() {
    // 12 deg/tick at 30 fps = full revolution per second. Snappy
    // enough to feel "active", slow enough that the icon shape
    // stays readable through the rotation.
    angle_ = std::fmod(angle_ + 12.0, 360.0);
    QTransform t;
    t.rotate(angle_);
    QPixmap rotated = basePixmap_.transformed(t, Qt::SmoothTransformation);
    // QPixmap::transformed grows the bounding box to fit the rotated
    // shape; for a square source the diagonal can be up to ~1.41x
    // the side. The QToolButton scales the icon to fit its iconSize
    // box automatically when we hand it a QIcon, so visual size
    // stays consistent across angles.
    setIcon(QIcon(rotated));
}

}  // namespace fc::ui
