// VirtualKeyboard: a uinput keyboard with the US layout ("Barry Launcher
// Keys" by default). The Trackpad app's keys type on the top screen with
// it; in Desktop Mode the bottom keyboard's shortcuts (Ctrl+C, ...) go
// through one, a real keyboard to KWin, so the focused window gets the
// modifiers. Characters the US layout cannot type are skipped.
//
// The device lives while this object is active and complete.
#pragma once

#include "uinputdevice.h"

#include <QObject>
#include <QQmlParserStatus>
#include <QStringList>
#include <QtQml/qqmlregistration.h>
#include <memory>
#include <mutex>

class VirtualKeyboard : public QObject, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_ELEMENT
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

public:
    explicit VirtualKeyboard(QObject *parent = nullptr);
    ~VirtualKeyboard() override;

    QString name() const { return m_name; }
    void setName(const QString &name);
    bool active() const { return m_active; }
    void setActive(bool active);
    bool ready() const { return m_device != nullptr; }

    void classBegin() override {}
    void componentComplete() override;

    // Whether every character of text is on the US layout (the rest are
    // skipped).
    Q_INVOKABLE bool canType(const QString &text) const;
    // Returns how many characters it could not type.
    Q_INVOKABLE int type(const QString &text);
    // A named key: BackSpace, Return, Escape, Tab, arrows, Home, End,
    // Prior, Next, Delete, space, F1-F12.
    Q_INVOKABLE bool key(const QString &name);
    // Backspace back times, then type text; returns the characters skipped.
    Q_INVOKABLE int replace(int back, const QString &text);
    // Modifiers (ctrl, shift, alt, super), then a key: ["ctrl", "c"].
    Q_INVOKABLE bool combo(const QStringList &keys);
    Q_INVOKABLE void releaseAll();

signals:
    void nameChanged();
    void activeChanged();
    void readyChanged();

private:
    void update();
    void tap(int code, const std::vector<int> &mods);

    QString m_name = QStringLiteral("Barry Launcher Keys");
    bool m_active = true;
    bool m_complete = false;
    std::mutex m_lock;
    std::unique_ptr<barry::UinputDevice> m_device;
};
