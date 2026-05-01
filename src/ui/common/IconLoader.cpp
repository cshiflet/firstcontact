#include "IconLoader.h"

#include <QApplication>
#include <QFile>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSvgRenderer>

namespace fc::ui {

namespace {

constexpr int kRenderSize = 32;   // 2x for HiDPI; QIcon scales as needed.

QIcon renderTinted(const QByteArray& svg, const QColor& color) {
    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) return {};

    QPixmap px(kRenderSize, kRenderSize);
    px.fill(Qt::transparent);

    QPainter painter(&px);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer.render(&painter);
    painter.end();

    // Composite the desired colour over the rendered pixmap, masked by the
    // pixmap's existing alpha. This recolors the icon in place — we don't
    // care about the original stroke colour because we're just using the
    // alpha channel as the icon shape.
    QPainter recolor(&px);
    recolor.setCompositionMode(QPainter::CompositionMode_SourceIn);
    recolor.fillRect(px.rect(), color);
    recolor.end();

    return QIcon(px);
}

}  // namespace

QIcon IconLoader::tinted(const QString& resourceName, const QColor& color) {
    QFile f(QStringLiteral(":/icons/symbolic/") + resourceName);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return renderTinted(f.readAll(), color);
}

QIcon IconLoader::themed(const QString& resourceName) {
    const QColor fg = qApp->palette().color(QPalette::WindowText);
    return tinted(resourceName, fg);
}

}  // namespace fc::ui
