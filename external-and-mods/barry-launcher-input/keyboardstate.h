// KeyboardState: what barry_launcher_shelld says about the keyboards, for
// the QML that shows them.
//
// bottom: the bottom screen's own keyboard. shelld shows it when a text
// field gets focus; shown, atTop (the field is in the lower half, so the
// keys go up top) and curtain (an app opening under the cover: its name)
// follow shelld as it says so (ShellState's keyboard). hide() puts it away
// by hand. A controller is reset each time the keyboard comes up.
//
// followTop: the Trackpad app's keyboard. shelld asks it up (a click in a
// top-screen text field) or down (ShellState's top-keyboard);
// topKeyboardAsked says which.
#pragma once

#include "keyboardcontroller.h"

#include <QObject>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

class QNetworkAccessManager;

class KeyboardState : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool bottom READ bottom WRITE setBottom NOTIFY settingsChanged)
    Q_PROPERTY(bool followTop READ followTop WRITE setFollowTop NOTIFY settingsChanged)
    Q_PROPERTY(KeyboardController *controller MEMBER m_controller NOTIFY settingsChanged)
    // The keys' height (window pixels): shelld pushes the apps up by it.
    Q_PROPERTY(int panelHeight READ panelHeight WRITE setPanelHeight NOTIFY settingsChanged)
    Q_PROPERTY(bool shown READ shown NOTIFY stateChanged)
    Q_PROPERTY(bool atTop READ atTop NOTIFY stateChanged)
    Q_PROPERTY(QString curtain READ curtain NOTIFY stateChanged)

public:
    explicit KeyboardState(QObject *parent = nullptr);
    ~KeyboardState() override;

    bool bottom() const { return m_bottom; }
    void setBottom(bool bottom);
    bool followTop() const { return m_followTop; }
    void setFollowTop(bool follow);
    int panelHeight() const { return m_panelHeight; }
    void setPanelHeight(int height);
    bool shown() const { return m_shown; }
    bool atTop() const { return m_atTop; }
    QString curtain() const { return m_curtain; }

    // The keyboard's own hide: its key, a tap beside it.
    Q_INVOKABLE void hide();

signals:
    void settingsChanged();
    void stateChanged();
    void topKeyboardAsked(bool up);

private:
    void takeBottom();
    void takeTop();

    bool m_bottom = false;
    bool m_followTop = false;
    QPointer<KeyboardController> m_controller;
    int m_panelHeight = 0;
    bool m_shown = false;
    bool m_atTop = false;
    QString m_curtain;
    int m_topSerial = -1;
    QNetworkAccessManager *m_net = nullptr;
};
