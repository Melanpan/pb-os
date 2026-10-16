// Typer: where the bottom screen's own keyboard (Keyboard.qml) types.
//
// Game Mode: into the focused window of the bottom screen's gamescope,
// through XTest. The keyboard window is a gamescope overlay that leaves X
// keyboard focus on the app underneath.
//
// Desktop Mode (desktop: true): into the active text field through KWin's
// input method (barry_launcher_imd, over its socket); with no field active,
// through XTest to Xwayland. Shortcuts (combo) go through keyboard, a
// VirtualKeyboard, a real keyboard to KWin, so the focused window gets the
// modifiers.
#pragma once

#include "virtualkeyboard.h"
#include "xtesttyper.h"

#include <QObject>
#include <QPointer>
#include <QSocketNotifier>
#include <QStringList>
#include <QTimer>
#include <QtQml/qqmlregistration.h>
#include <memory>

class Typer : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool desktop READ desktop WRITE setDesktop NOTIFY desktopChanged)
    Q_PROPERTY(VirtualKeyboard *keyboard READ keyboard WRITE setKeyboard NOTIFY keyboardChanged)
    // The X display XTest types on: a name (":1"), "$NAME" for the one an
    // environment variable names (the top screen's, $BARRY_TOP_DISPLAY), or
    // "" for DISPLAY's.
    Q_PROPERTY(QString display READ display WRITE setDisplay NOTIFY displayChanged)
    // Desktop Mode: a text field is active in KWin's input method.
    Q_PROPERTY(bool fieldActive READ fieldActive NOTIFY fieldActiveChanged)

public:
    explicit Typer(QObject *parent = nullptr);
    ~Typer() override;

    bool desktop() const { return m_desktop; }
    QString display() const { return m_display; }
    void setDisplay(const QString &display);
    void setDesktop(bool desktop);
    VirtualKeyboard *keyboard() const { return m_keyboard; }
    void setKeyboard(VirtualKeyboard *keyboard);
    bool fieldActive() const { return m_fieldActive; }

    Q_INVOKABLE bool type(const QString &text);
    // BackSpace, Return, Tab, Escape, Delete, space, arrows, Home, End,
    // Prior, Next
    Q_INVOKABLE bool key(const QString &name);
    // Backspace back times, then type text, as one change where it can.
    Q_INVOKABLE bool replace(int back, const QString &text);
    // Modifiers (ctrl, shift, alt, super), then a key: ["ctrl", "c"].
    Q_INVOKABLE bool combo(const QStringList &keys);

signals:
    void desktopChanged();
    void displayChanged();
    void keyboardChanged();
    void fieldActiveChanged();

private:
    bool im() const { return m_desktop && m_fieldActive && m_imFd >= 0; }
    bool imSend(const QString &line);
    void imConnect();
    void imClose();
    void imRead();
    void setFieldActive(bool active);
    barry::XTestTyper *x();

    bool m_desktop = false;
    QString m_display;
    QPointer<VirtualKeyboard> m_keyboard;
    bool m_fieldActive = false;
    int m_imFd = -1;
    std::unique_ptr<QSocketNotifier> m_imNotifier;
    QByteArray m_imLine;
    QTimer m_imRetry;
    std::unique_ptr<barry::XTestTyper> m_x;  // opened on first use
};
