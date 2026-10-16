#include "virtualkeyboard.h"

#include "keymap.h"

#include <linux/input-event-codes.h>

#include <QDebug>

namespace {
constexpr int MAX_TEXT = 1000;  // characters in one call
constexpr int MAX_BACK = 100;
}  // namespace

VirtualKeyboard::VirtualKeyboard(QObject *parent) : QObject(parent) {}

VirtualKeyboard::~VirtualKeyboard() = default;

void VirtualKeyboard::setName(const QString &name)
{
    if (name == m_name)
        return;
    m_name = name;
    emit nameChanged();
    if (m_device) {
        {
            std::lock_guard<std::mutex> hold(m_lock);
            m_device.reset();
        }
        update();
    }
}

void VirtualKeyboard::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    emit activeChanged();
    update();
}

void VirtualKeyboard::componentComplete()
{
    m_complete = true;
    update();
}

void VirtualKeyboard::update()
{
    const bool want = m_complete && m_active;
    {
        std::lock_guard<std::mutex> hold(m_lock);
        if (want == (m_device != nullptr))
            return;
        if (want) {
            m_device = std::make_unique<barry::UinputDevice>(m_name.toUtf8(), barry::allKeys(), std::vector<int>{});
            if (!m_device->ok()) {
                qWarning().noquote() << "Barry.Input:" << m_device->error();
                m_device.reset();
            }
        } else {
            m_device.reset();
        }
    }
    emit readyChanged();
}

void VirtualKeyboard::tap(int code, const std::vector<int> &mods)
{
    for (int m : mods)
        m_device->key(m, true);
    m_device->key(code, true);
    m_device->key(code, false);
    for (auto it = mods.rbegin(); it != mods.rend(); ++it)
        m_device->key(*it, false);
}

bool VirtualKeyboard::canType(const QString &text) const
{
    for (uint c : text.toUcs4())
        if (!barry::usChar(char32_t(c)))
            return false;
    return true;
}

int VirtualKeyboard::type(const QString &text)
{
    return replace(0, text);
}

int VirtualKeyboard::replace(int back, const QString &text)
{
    const QList<uint> chars = text.left(MAX_TEXT).toUcs4();
    std::lock_guard<std::mutex> hold(m_lock);
    if (!m_device)
        return int(chars.size());
    for (int i = 0; i < qBound(0, back, MAX_BACK); i++)
        tap(KEY_BACKSPACE, {});
    int skipped = 0;
    for (uint c : chars) {
        const auto stroke = barry::usChar(char32_t(c));
        if (!stroke) {
            skipped++;
            continue;
        }
        tap(stroke->code, stroke->shift ? std::vector<int>{KEY_LEFTSHIFT} : std::vector<int>{});
    }
    return skipped;
}

bool VirtualKeyboard::key(const QString &name)
{
    const int code = barry::namedKey(name);
    std::lock_guard<std::mutex> hold(m_lock);
    if (!m_device || code < 0)
        return false;
    tap(code, {});
    return true;
}

bool VirtualKeyboard::combo(const QStringList &keys)
{
    if (keys.size() < 2 || keys.size() > 4)
        return false;
    std::vector<int> mods;
    for (int i = 0; i < keys.size() - 1; i++) {
        const int m = barry::modifierKey(keys[i]);
        if (m < 0)
            return false;
        mods.push_back(m);
    }
    const QString &last = keys.last();
    int code = barry::namedKey(last);
    if (code < 0) {
        const QList<uint> c = last.toLower().toUcs4();
        const auto stroke = c.size() == 1 ? barry::usChar(char32_t(c[0])) : std::nullopt;
        if (!stroke)
            return false;
        code = stroke->code;
    }
    std::lock_guard<std::mutex> hold(m_lock);
    if (!m_device)
        return false;
    tap(code, mods);
    return true;
}

void VirtualKeyboard::releaseAll()
{
    std::lock_guard<std::mutex> hold(m_lock);
    if (m_device)
        m_device->releaseAll();
}
