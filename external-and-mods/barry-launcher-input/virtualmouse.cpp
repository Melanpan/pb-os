#include "virtualmouse.h"

#include <linux/input-event-codes.h>

#include <QDebug>
#include <cmath>

namespace {

// Scroll: one notch is 120 hi-res units.
constexpr int NOTCH = 120;
// Game Mode's gamescope hides a pointer that has not moved for 3 s
// (--hide-cursor-delay 3000), and a hidden pointer's wheel never reaches
// Steam: a scroll after a still spell first nudges the pointer there and back.
constexpr auto WAKE_POINTER = std::chrono::milliseconds(2000);

int buttonCode(const QString &name)
{
    if (name == QLatin1String("left"))
        return BTN_LEFT;
    if (name == QLatin1String("right"))
        return BTN_RIGHT;
    if (name == QLatin1String("middle"))
        return BTN_MIDDLE;
    return -1;
}

}  // namespace

namespace barry {

MouseCore::MouseCore(const QByteArray &name)
    : m_device(name, {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE},
               {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES})
{
}

void MouseCore::move(double dx, double dy)
{
    if (!std::isfinite(dx) || !std::isfinite(dy))
        return;
    std::lock_guard<std::mutex> hold(m_lock);
    m_rest[0] += dx;
    m_rest[1] += dy;
    const int ix = int(m_rest[0]), iy = int(m_rest[1]);
    if (!ix && !iy)
        return;
    m_rest[0] -= ix;
    m_rest[1] -= iy;
    m_device.emitEvents({{EV_REL, REL_X, ix}, {EV_REL, REL_Y, iy}});
    m_movedAt = std::chrono::steady_clock::now();
}

void MouseCore::scroll(double dx, double dy)
{
    if (!std::isfinite(dx) || !std::isfinite(dy))
        return;
    std::lock_guard<std::mutex> hold(m_lock);
    std::vector<Event> events;
    // Wheel up is positive; a finger moving up scrolls the page down.
    const struct { int hi, lo; double amount; } axes[2] = {
        {REL_HWHEEL_HI_RES, REL_HWHEEL, dx}, {REL_WHEEL_HI_RES, REL_WHEEL, -dy}};
    for (int i = 0; i < 2; i++) {
        const int units = int(std::lround(axes[i].amount * NOTCH));
        if (!units)
            continue;
        events.push_back({EV_REL, axes[i].hi, units});
        // Clients without hi-res scrolling see whole notches.
        m_wheel[i] += units;
        const int notches = m_wheel[i] / NOTCH;
        if (notches) {
            m_wheel[i] -= notches * NOTCH;
            events.push_back({EV_REL, axes[i].lo, notches});
        }
    }
    if (events.empty())
        return;
    const auto now = std::chrono::steady_clock::now();
    if (now - m_movedAt > WAKE_POINTER) {
        m_device.emitEvents({{EV_REL, REL_X, 1}});
        m_device.emitEvents({{EV_REL, REL_X, -1}});
        m_movedAt = now;
    }
    m_device.emitEvents(events);
}

void MouseCore::button(int code, bool down, bool up)
{
    std::lock_guard<std::mutex> hold(m_lock);
    if (down)
        m_device.key(code, true);
    if (up)
        m_device.key(code, false);
}

void MouseCore::releaseAll()
{
    std::lock_guard<std::mutex> hold(m_lock);
    m_device.releaseAll();
}

}  // namespace barry

VirtualMouse::VirtualMouse(QObject *parent) : QObject(parent) {}

VirtualMouse::~VirtualMouse() = default;

void VirtualMouse::setName(const QString &name)
{
    if (name == m_name)
        return;
    m_name = name;
    emit nameChanged();
    if (m_complete)
        create();
}

void VirtualMouse::componentComplete()
{
    m_complete = true;
    create();
}

void VirtualMouse::create()
{
    m_core.reset();  // the old device first: one at a time under a name
    auto core = std::make_shared<barry::MouseCore>(m_name.toUtf8());
    if (core->ok())
        m_core = core;
    else
        qWarning().noquote() << "Barry.Input:" << core->error();
    emit readyChanged();
}

void VirtualMouse::move(qreal dx, qreal dy)
{
    if (m_core)
        m_core->move(dx, dy);
}

void VirtualMouse::scroll(qreal dx, qreal dy)
{
    if (m_core)
        m_core->scroll(dx, dy);
}

void VirtualMouse::press(const QString &b)
{
    const int code = buttonCode(b);
    if (m_core && code >= 0)
        m_core->button(code, true, false);
}

void VirtualMouse::release(const QString &b)
{
    const int code = buttonCode(b);
    if (m_core && code >= 0)
        m_core->button(code, false, true);
}

void VirtualMouse::click(const QString &b)
{
    const int code = buttonCode(b);
    if (m_core && code >= 0)
        m_core->button(code, true, true);
}

void VirtualMouse::releaseAll()
{
    if (m_core)
        m_core->releaseAll();
}
