#include "SpinningToolButton.h"

#include <QPainter>
#include <QPixmap>
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

    // Render the rotated icon into a fixed-size QPixmap matching
    // iconSize() so the apparent visual size stays CONSTANT through
    // the rotation. QPixmap::transformed grows the bounding box to
    // fit the rotated shape — at 45° the diagonal of a square is
    // 1.41x the side, so QToolButton then scales 1.41x-sized pixmaps
    // back into iconSize, producing a "pulsing" effect (icon grows
    // and shrinks once per revolution). Drawing into a same-size
    // canvas with center-rotation clips to a constant footprint.
    const QSize sz = iconSize();
    if (sz.isEmpty() || basePixmap_.isNull()) {
        setIcon(baseIcon_);
        return;
    }
    QPixmap rotated(sz);
    rotated.setDevicePixelRatio(basePixmap_.devicePixelRatio());
    rotated.fill(Qt::transparent);

    QPainter p(&rotated);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    // Rotate around the canvas centre, then draw the basePixmap
    // (already iconSize-sized) centred. The basePixmap's corners
    // sweep outside the canvas at non-axis angles and get clipped —
    // exactly what we want for a constant footprint.
    p.translate(sz.width() / 2.0, sz.height() / 2.0);
    p.rotate(angle_);
    p.translate(-basePixmap_.width() / (2.0 * basePixmap_.devicePixelRatio()),
                -basePixmap_.height() / (2.0 * basePixmap_.devicePixelRatio()));
    p.drawPixmap(0, 0, basePixmap_);
    p.end();

    setIcon(QIcon(rotated));
}

}  // namespace fc::ui
