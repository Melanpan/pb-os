#include "heattrail.h"

#include <QColor>
#include <QEventPoint>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTouchEvent>
#include <algorithm>
#include <cmath>

namespace {

// Item pixels per heat cell: coarse, then drawn smoothly scaled.
constexpr qreal CELL = 6;
constexpr int FRAME_MS = 16;
// Heat a still finger adds per second, and a moving one per stamp.
constexpr float HOLD_RATE = 0.9f;
constexpr float STROKE = 0.22f;
// Share of a cell's heat that spreads to its neighbors per second.
constexpr float SPREAD = 0.9f;
// Below this everywhere: cold, and the timer stops.
constexpr float COLD = 0.008f;
// The mouse's touch id.
constexpr int MOUSE_ID = -1;

}  // namespace

HeatTrail::HeatTrail(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    m_timer.setInterval(FRAME_MS);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &HeatTrail::tick);
}

HeatTrail::~HeatTrail()
{
    if (m_source)
        m_source->removeEventFilter(this);
}

void HeatTrail::setSource(QQuickItem *source)
{
    if (source == m_source)
        return;
    if (m_source)
        m_source->removeEventFilter(this);
    m_source = source;
    m_fingers.clear();
    if (m_source)
        m_source->installEventFilter(this);
    emit sourceChanged();
}

void HeatTrail::setColors(const QStringList &colors)
{
    if (colors == m_colors)
        return;
    m_colors = colors;
    buildPalette();
    update();
    emit colorsChanged();
}

void HeatTrail::buildPalette()
{
    QVector<QColor> stops;
    for (const QString &c : m_colors) {
        const QColor color = QColor::fromString(c);
        if (color.isValid())
            stops << color;
    }
    for (int i = 0; i < 256; i++) {
        if (stops.size() < 2) {
            m_palette[i] = 0;
            continue;
        }
        const float t = i / 255.0f;
        // Fades in from nothing over the coolest tenth.
        const float alpha = std::min(1.0f, t / 0.1f) * 0.95f;
        const float pos = t * (stops.size() - 1);
        const int a = std::min(int(pos), int(stops.size()) - 2);
        const float f = pos - a;
        const QColor &c0 = stops[a], &c1 = stops[a + 1];
        const float r = c0.redF() + (c1.redF() - c0.redF()) * f;
        const float g = c0.greenF() + (c1.greenF() - c0.greenF()) * f;
        const float b = c0.blueF() + (c1.blueF() - c0.blueF()) * f;
        m_palette[i] = qRgba(int(r * alpha * 255), int(g * alpha * 255), int(b * alpha * 255), int(alpha * 255));
    }
}

void HeatTrail::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        resizeGrid();
}

void HeatTrail::resizeGrid()
{
    m_w = std::max(1, int(std::ceil(width() / CELL)));
    m_h = std::max(1, int(std::ceil(height() / CELL)));
    m_heat.fill(0.0f, m_w * m_h);
    m_spare.fill(0.0f, m_w * m_h);
    m_image = QImage(m_w, m_h, QImage::Format_ARGB32_Premultiplied);
    m_image.fill(Qt::transparent);
    update();
}

void HeatTrail::clear()
{
    m_heat.fill(0.0f);
    m_image.fill(Qt::transparent);
    if (m_timer.isActive() && m_fingers.isEmpty()) {
        m_timer.stop();
        emit warmChanged();
    }
    update();
}

QSizeF HeatTrail::contact(QSizeF reported) const
{
    // A touchscreen that reports a contact: that, but never a pinpoint.
    if (reported.width() > 4 && reported.height() > 4)
        return reported.expandedTo(QSizeF(m_fingerSize * 0.5, m_fingerSize * 0.5));
    return QSizeF(m_fingerSize, m_fingerSize * 1.25);
}

bool HeatTrail::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_source || m_colors.size() < 2 || !isVisible())
        return false;
    switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel: {
        const auto *touch = static_cast<QTouchEvent *>(event);
        if (event->type() == QEvent::TouchCancel) {
            m_fingers.clear();
            break;
        }
        for (const QEventPoint &p : touch->points()) {
            if (p.state() == QEventPoint::Released) {
                lift(p.id());
                continue;
            }
            const QPointF at = mapFromItem(m_source, p.position());
            touchAt(p.id(), at, contact(p.ellipseDiameters()), p.state() == QEventPoint::Pressed);
        }
        break;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        // Touches come as mouse events too: only a real mouse here.
        if (mouse->source() != Qt::MouseEventNotSynthesized)
            break;
        if (event->type() == QEvent::MouseButtonRelease) {
            lift(MOUSE_ID);
            break;
        }
        if (event->type() == QEvent::MouseMove && !(mouse->buttons() & Qt::LeftButton))
            break;
        touchAt(MOUSE_ID, mapFromItem(m_source, mouse->position()), contact({}),
                event->type() == QEvent::MouseButtonPress);
        break;
    }
    default:
        break;
    }
    return false;  // source gets every event
}

void HeatTrail::touchAt(int id, QPointF at, QSizeF size, bool down)
{
    auto it = m_fingers.find(id);
    if (down || it == m_fingers.end()) {
        m_fingers.insert(id, {at, size});
        stamp(at, size, STROKE);
    } else {
        // Along the way, so a quick stroke leaves a streak, not dots.
        const QPointF from = it->at;
        it->size = size;
        // Held still (a report because another finger moved): the hold
        // warms it, in tick().
        if (QLineF(from, at).length() < 1)
            return;
        const qreal step = std::max<qreal>(CELL, size.width() * 0.25);
        const int n = std::max(1, int(std::ceil(QLineF(from, at).length() / step)));
        for (int i = 1; i <= n; i++)
            stamp(from + (at - from) * (qreal(i) / n), size, STROKE / std::sqrt(float(n)));
        it->at = at;
    }
    wake();
}

void HeatTrail::lift(int id)
{
    m_fingers.remove(id);
}

void HeatTrail::stamp(QPointF at, QSizeF size, float amount)
{
    if (m_heat.isEmpty())
        return;
    const qreal rx = size.width() / 2 / CELL, ry = size.height() / 2 / CELL;
    const qreal cx = at.x() / CELL, cy = at.y() / CELL;
    const int x0 = std::max(0, int(cx - rx)), x1 = std::min(m_w - 1, int(cx + rx) + 1);
    const int y0 = std::max(0, int(cy - ry)), y1 = std::min(m_h - 1, int(cy + ry) + 1);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            const qreal dx = (x + 0.5 - cx) / rx, dy = (y + 0.5 - cy) / ry;
            const qreal d = dx * dx + dy * dy;
            if (d >= 1)
                continue;
            // Warmest at the middle of the contact, soft at its edge.
            const float fall = float((1 - d) * (1 - d));
            float &h = m_heat[y * m_w + x];
            h = std::min(1.0f, h + amount * fall);
        }
    }
}

void HeatTrail::wake()
{
    if (m_timer.isActive())
        return;
    m_clock.start();
    m_timer.start();
    emit warmChanged();
}

void HeatTrail::tick()
{
    const float dt = std::min<qint64>(m_clock.restart(), 100) / 1000.0f;
    // Still fingers keep warming.
    for (const Finger &f : std::as_const(m_fingers))
        stamp(f.at, f.size, HOLD_RATE * dt);

    // Spread to the four neighbors, and cool.
    const float spread = std::min(0.25f, SPREAD * dt);
    const float keep = float(std::exp(-dt / std::max<qreal>(0.1, m_fade)));
    float hottest = 0;
    for (int y = 0; y < m_h; y++) {
        for (int x = 0; x < m_w; x++) {
            const int i = y * m_w + x;
            const float h = m_heat[i];
            const float l = x > 0 ? m_heat[i - 1] : h, r = x < m_w - 1 ? m_heat[i + 1] : h;
            const float u = y > 0 ? m_heat[i - m_w] : h, d = y < m_h - 1 ? m_heat[i + m_w] : h;
            const float v = (h + spread * ((l + r + u + d) / 4 - h)) * keep;
            m_spare[i] = v;
            hottest = std::max(hottest, v);
        }
    }
    m_heat.swap(m_spare);

    QRgb *px = reinterpret_cast<QRgb *>(m_image.bits());
    for (int i = 0; i < m_w * m_h; i++)
        px[i] = m_palette[std::clamp(int(m_heat[i] * 255), 0, 255)];
    update();

    if (m_fingers.isEmpty() && hottest < COLD) {
        clear();
    }
}

void HeatTrail::paint(QPainter *painter)
{
    if (m_image.isNull() || m_colors.size() < 2)
        return;
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->setRenderHint(QPainter::Antialiasing);
    if (m_radius > 0) {
        QPainterPath clip;
        clip.addRoundedRect(QRectF(0, 0, width(), height()), m_radius, m_radius);
        painter->setClipPath(clip);
    }
    painter->drawImage(QRectF(0, 0, m_w * CELL, m_h * CELL), m_image);
}
