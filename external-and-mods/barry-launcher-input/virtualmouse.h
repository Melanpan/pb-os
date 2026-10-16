// VirtualMouse: a uinput mouse ("Barry Launcher Trackpad") that only the
// top screen sees. In Game Mode the bottom screen's gamescope takes no input
// devices of its own (its touches come over the lease socket), so this
// reaches only the top screen's gamescope, which hands it to Steam or the
// game; in Desktop Mode KWin takes it like any mouse.
//
// The device lives as long as this object: made when the QML that declares
// it is complete, gone (and with it anything it held down) when it goes.
// TouchTrackpad's reader thread moves it too, through the shared core, so
// it is safe whichever of the two QML destroys first.
#pragma once

#include "uinputdevice.h"

#include <QObject>
#include <QQmlParserStatus>
#include <QtQml/qqmlregistration.h>
#include <chrono>
#include <memory>
#include <mutex>

namespace barry {

class MouseCore {
public:
    explicit MouseCore(const QByteArray &name);
    bool ok() const { return m_device.ok(); }
    QString error() const { return m_device.error(); }

    void move(double dx, double dy);
    void scroll(double dx, double dy);
    // BTN_LEFT, BTN_RIGHT, BTN_MIDDLE
    void button(int code, bool down, bool up);
    void releaseAll();

private:
    std::mutex m_lock;
    UinputDevice m_device;
    double m_rest[2] = {0, 0};  // sub-pixel motion carried to the next move
    int m_wheel[2] = {0, 0};    // hi-res units sent since the last whole notch
    std::chrono::steady_clock::time_point m_movedAt;
};

}  // namespace barry

class VirtualMouse : public QObject, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_ELEMENT
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

public:
    explicit VirtualMouse(QObject *parent = nullptr);
    ~VirtualMouse() override;

    QString name() const { return m_name; }
    void setName(const QString &name);
    bool ready() const { return m_core != nullptr; }
    std::shared_ptr<barry::MouseCore> core() const { return m_core; }

    void classBegin() override {}
    void componentComplete() override;

    // Pointer motion in top-screen pixels; fractions add up.
    Q_INVOKABLE void move(qreal dx, qreal dy);
    // Scroll in notches; fractions add up. Positive dy scrolls down.
    Q_INVOKABLE void scroll(qreal dx, qreal dy);
    // "left", "right" or "middle"
    Q_INVOKABLE void press(const QString &button);
    Q_INVOKABLE void release(const QString &button);
    Q_INVOKABLE void click(const QString &button);
    Q_INVOKABLE void releaseAll();

signals:
    void nameChanged();
    void readyChanged();

private:
    void create();

    QString m_name = QStringLiteral("Barry Launcher Trackpad");
    bool m_complete = false;
    std::shared_ptr<barry::MouseCore> m_core;
};
