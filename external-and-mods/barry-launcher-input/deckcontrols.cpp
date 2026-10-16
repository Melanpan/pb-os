#include "deckcontrols.h"

#include <linux/input-event-codes.h>

#include <QDebug>
#include <QHash>
#include <cmath>

namespace {

// The pads' resolution: finer than any finger.
constexpr int PAD_MAX = 4095;

const QHash<QString, int> &buttonCodes()
{
    // InputPlumber reads BTN_TRIGGER_HAPPY5-8 as R4 R5 L4 L5 by itself;
    // barry_deck_buttons maps 11/12 to the trackpads' clicks.
    static const QHash<QString, int> codes = {
        {"R4", BTN_TRIGGER_HAPPY5}, {"R5", BTN_TRIGGER_HAPPY6},
        {"L4", BTN_TRIGGER_HAPPY7}, {"L5", BTN_TRIGGER_HAPPY8},
        {"LeftPadClick", BTN_TRIGGER_HAPPY11}, {"RightPadClick", BTN_TRIGGER_HAPPY12},
    };
    return codes;
}

int toPad(qreal fraction)
{
    return int(std::lround(qBound(0.0, std::isfinite(fraction) ? fraction : 0.0, 1.0) * PAD_MAX));
}

}  // namespace

DeckPad::DeckPad(QObject *parent) : QObject(parent) {}

DeckPad::~DeckPad() = default;

void DeckPad::setSide(const QString &side)
{
    if (side == m_side || (side != QLatin1String("left") && side != QLatin1String("right")))
        return;
    m_side = side;
    emit sideChanged();
    if (m_complete)
        create();
}

void DeckPad::componentComplete()
{
    m_complete = true;
    create();
}

void DeckPad::create()
{
    m_device.reset();
    if (m_touching) {
        m_touching = false;
        emit touchingChanged();
    }
    const QByteArray name = m_side == QLatin1String("left") ? "Barry Deck Left Pad" : "Barry Deck Right Pad";
    // A touchscreen to udev (ID_INPUT_TOUCHSCREEN: direct, with BTN_TOUCH and
    // position axes), so InputPlumber takes it with its touchscreen driver.
    m_device = std::make_unique<barry::UinputDevice>(
        name, std::vector<int>{BTN_TOUCH}, std::vector<int>{},
        std::vector<barry::Axis>{{ABS_X, 0, PAD_MAX}, {ABS_Y, 0, PAD_MAX}, {ABS_MT_SLOT, 0, 0},
                                 {ABS_MT_TRACKING_ID, 0, 65535}, {ABS_MT_POSITION_X, 0, PAD_MAX},
                                 {ABS_MT_POSITION_Y, 0, PAD_MAX}},
        std::vector<int>{INPUT_PROP_DIRECT});
    if (!m_device->ok()) {
        qWarning().noquote() << "Barry.Input:" << m_device->error();
        m_device.reset();
    }
    emit readyChanged();
}

void DeckPad::touch(qreal x, qreal y)
{
    if (!m_device)
        return;
    const int px = toPad(x), py = toPad(y);
    std::vector<barry::Event> events{{EV_ABS, ABS_MT_SLOT, 0}};
    if (!m_touching) {
        m_trackingId = (m_trackingId + 1) % 65536;
        events.push_back({EV_ABS, ABS_MT_TRACKING_ID, m_trackingId});
    }
    events.push_back({EV_ABS, ABS_MT_POSITION_X, px});
    events.push_back({EV_ABS, ABS_MT_POSITION_Y, py});
    if (!m_touching)
        events.push_back({EV_KEY, BTN_TOUCH, 1});
    events.push_back({EV_ABS, ABS_X, px});
    events.push_back({EV_ABS, ABS_Y, py});
    m_device->emitEvents(events);
    if (!m_touching) {
        m_touching = true;
        emit touchingChanged();
    }
}

void DeckPad::release()
{
    if (!m_device || !m_touching)
        return;
    m_device->emitEvents({{EV_ABS, ABS_MT_SLOT, 0}, {EV_ABS, ABS_MT_TRACKING_ID, -1}, {EV_KEY, BTN_TOUCH, 0}});
    m_touching = false;
    emit touchingChanged();
}

DeckButtons::DeckButtons(QObject *parent) : QObject(parent) {}

DeckButtons::~DeckButtons() = default;

void DeckButtons::componentComplete()
{
    std::vector<int> keys;
    for (int code : buttonCodes())
        keys.push_back(code);
    m_device = std::make_unique<barry::UinputDevice>("Barry Deck Buttons", keys, std::vector<int>{});
    if (!m_device->ok()) {
        qWarning().noquote() << "Barry.Input:" << m_device->error();
        m_device.reset();
    }
    emit readyChanged();
}

bool DeckButtons::press(const QString &button)
{
    const int code = buttonCodes().value(button, -1);
    if (!m_device || code < 0)
        return false;
    m_device->key(code, true);
    return true;
}

bool DeckButtons::release(const QString &button)
{
    const int code = buttonCodes().value(button, -1);
    if (!m_device || code < 0)
        return false;
    m_device->key(code, false);
    return true;
}

void DeckButtons::releaseAll()
{
    if (m_device)
        m_device->releaseAll();
}
