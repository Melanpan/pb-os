#include "trackpadarea.h"

#include "haptics.h"

#include <QMouseEvent>
#include <QTouchEvent>
#include <chrono>
#include <cmath>

namespace {

double monotonic()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr int MOUSE_ID = -100;  // a mouse stands in for one finger

}  // namespace

// --- TrackpadArea -------------------------------------------------------

TrackpadArea::TrackpadArea(QQuickItem *parent) : QQuickItem(parent)
{
    setAcceptTouchEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton);
    m_due.setSingleShot(true);
    connect(&m_due, &QTimer::timeout, this, [this] {
        if (m_gesture)
            m_gesture->due(monotonic());
        arm();
    });
}

TrackpadArea::~TrackpadArea()
{
    reset();
}

void TrackpadArea::setMouse(VirtualMouse *mouse)
{
    if (mouse == m_mouse)
        return;
    reset();
    if (m_mouse)
        disconnect(m_mouse, nullptr, this, nullptr);
    m_mouse = mouse;
    m_gesture.reset();
    // A new device for the mouse (its name changed): a new gesture for it.
    if (m_mouse)
        connect(m_mouse, &VirtualMouse::readyChanged, this, [this] {
            reset();
            m_gesture.reset();
        });
    emit mouseChanged();
}

barry::TrackpadGesture *TrackpadArea::gesture()
{
    if (m_gesture || !m_mouse || !m_mouse->core())
        return m_gesture.get();
    m_core = m_mouse->core();
    barry::TrackpadGesture::Callbacks cb;
    cb.clicked = [this] {
        Haptics::instance()->click();
        emit clicked();
    };
    cb.touching = [this](bool touching) {
        m_touching = touching;
        emit touchingChanged();
    };
    cb.speed = [this] { return double(m_speed); };
    m_gesture = std::make_unique<barry::TrackpadGesture>(m_core, cb);
    return m_gesture.get();
}

void TrackpadArea::reset()
{
    m_due.stop();
    if (m_gesture)
        m_gesture->reset();
}

void TrackpadArea::arm()
{
    m_due.stop();
    if (!m_gesture)
        return;
    if (const auto due = m_gesture->nextDue())
        m_due.start(std::max(0, int(std::ceil((*due - monotonic()) * 1000))));
}

void TrackpadArea::frame(const std::map<int, barry::TrackpadGesture::Point> &live, double stampSeconds)
{
    barry::TrackpadGesture *g = gesture();
    if (!g)
        return;
    g->frame(live, monotonic(), stampSeconds);
    arm();
}

void TrackpadArea::touchEvent(QTouchEvent *event)
{
    std::map<int, barry::TrackpadGesture::Point> live;
    const qreal s = m_s > 0 ? m_s : 1;
    for (const QEventPoint &p : event->points()) {
        if (p.state() != QEventPoint::Released)
            live[p.id()] = {p.position().x() / s, p.position().y() / s};
    }
    frame(live, double(event->timestamp()) / 1000.0);
    event->accept();
}

void TrackpadArea::touchUngrabEvent()
{
    // Qt lets go of every touch once it ends: only a touch taken away while
    // fingers are still down (cancelled) lifts them; a tap's click still
    // comes.
    if (m_gesture && m_gesture->active())
        reset();
}

void TrackpadArea::mousePressEvent(QMouseEvent *event)
{
    const qreal s = m_s > 0 ? m_s : 1;
    frame({{MOUSE_ID, {event->position().x() / s, event->position().y() / s}}}, double(event->timestamp()) / 1000.0);
    event->accept();
}

void TrackpadArea::mouseMoveEvent(QMouseEvent *event)
{
    mousePressEvent(event);
}

void TrackpadArea::mouseReleaseEvent(QMouseEvent *event)
{
    frame({}, double(event->timestamp()) / 1000.0);
    event->accept();
}

void TrackpadArea::mouseUngrabEvent()
{
    if (m_gesture && m_gesture->active())
        reset();
}

void TrackpadArea::itemChange(ItemChange change, const ItemChangeData &value)
{
    // Nothing stays held when the pad goes away.
    if (change == ItemVisibleHasChanged && !value.boolValue)
        reset();
    QQuickItem::itemChange(change, value);
}

// --- PressArea ----------------------------------------------------------

PressArea::PressArea(QQuickItem *parent) : QQuickItem(parent)
{
    setAcceptTouchEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton);
}

PressArea::~PressArea()
{
    up();
}

void PressArea::down(int finger)
{
    if (m_finger != NONE)
        return;
    m_finger = finger;
    Haptics::instance()->click();
    if (m_mouse)
        m_mouse->press(m_button);
    else if (m_deck)
        m_deck->press(m_button);
    emit pressedChanged();
}

void PressArea::up()
{
    if (m_finger == NONE)
        return;
    m_finger = NONE;
    if (m_mouse)
        m_mouse->release(m_button);
    else if (m_deck)
        m_deck->release(m_button);
    emit pressedChanged();
}

void PressArea::touchEvent(QTouchEvent *event)
{
    // Held by the first finger, until it lifts (wherever it goes).
    for (const QEventPoint &p : event->points()) {
        if (p.state() == QEventPoint::Pressed && m_finger == NONE)
            down(p.id());
        else if (p.state() == QEventPoint::Released && p.id() == m_finger)
            up();
    }
    event->accept();
}

void PressArea::touchUngrabEvent()
{
    up();
}

void PressArea::mousePressEvent(QMouseEvent *event)
{
    down(MOUSE_ID);
    event->accept();
}

void PressArea::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_finger == MOUSE_ID)
        up();
    event->accept();
}

void PressArea::mouseUngrabEvent()
{
    up();
}

void PressArea::itemChange(ItemChange change, const ItemChangeData &value)
{
    if (change == ItemVisibleHasChanged && !value.boolValue)
        up();
    QQuickItem::itemChange(change, value);
}

// --- DeckPadArea --------------------------------------------------------

DeckPadArea::DeckPadArea(QQuickItem *parent) : QQuickItem(parent)
{
    setAcceptTouchEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton);
}

void DeckPadArea::setPad(DeckPad *pad)
{
    if (pad == m_pad)
        return;
    lift();
    m_pad = pad;
    emit padChanged();
}

void DeckPadArea::follow(QPointF pos)
{
    m_x = width() > 0 ? qBound(0.0, pos.x() / width(), 1.0) : 0;
    m_y = height() > 0 ? qBound(0.0, pos.y() / height(), 1.0) : 0;
    if (m_pad)
        m_pad->touch(m_x, m_y);
    emit fingerChanged();
}

void DeckPadArea::lift()
{
    if (m_finger < 0)
        return;
    m_finger = -1;
    if (m_pad)
        m_pad->release();
    emit touchingChanged();
}

void DeckPadArea::touchEvent(QTouchEvent *event)
{
    // One finger: the first down, until it lifts.
    for (const QEventPoint &p : event->points()) {
        if (m_finger < 0 && p.state() == QEventPoint::Pressed) {
            m_finger = p.id();
            emit touchingChanged();
        }
        if (p.id() != m_finger)
            continue;
        if (p.state() == QEventPoint::Released)
            lift();
        else
            follow(p.position());
    }
    event->accept();
}

void DeckPadArea::touchUngrabEvent()
{
    lift();
}

void DeckPadArea::mousePressEvent(QMouseEvent *event)
{
    if (m_finger < 0) {
        m_finger = MOUSE_ID;
        emit touchingChanged();
    }
    follow(event->position());
    event->accept();
}

void DeckPadArea::mouseMoveEvent(QMouseEvent *event)
{
    if (m_finger == MOUSE_ID)
        follow(event->position());
    event->accept();
}

void DeckPadArea::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_finger == MOUSE_ID)
        lift();
    event->accept();
}

void DeckPadArea::itemChange(ItemChange change, const ItemChangeData &value)
{
    if (change == ItemVisibleHasChanged && !value.boolValue)
        lift();
    QQuickItem::itemChange(change, value);
}
