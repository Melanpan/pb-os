// Touch surfaces that do their own input handling, so the QML that places
// and draws them (and any skin) holds no input logic.
//
// TrackpadArea: the Trackpad app's pad. Its touches (and a mouse, for a
// test without a touchscreen) go to TrackpadGesture (gesture.h), the same
// gestures as Desktop Mode's TouchTrackpad, which move mouse.
//
// DeckPadArea: a Deck control's trackpad. Its first finger, as a fraction
// of the area, is pad's touch (DeckPad).
//
// PressArea: a button held down while a finger is on it (a mouse button of
// mouse, or a Deck button of deck), with a buzz as it goes down.
#pragma once

#include "deckcontrols.h"
#include "gesture.h"
#include "virtualmouse.h"

#include <QPointer>
#include <QQuickItem>
#include <QTimer>
#include <QtQml/qqmlregistration.h>
#include <memory>

class TrackpadArea : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(VirtualMouse *mouse READ mouse WRITE setMouse NOTIFY mouseChanged)
    // Pointer pixels per design pixel moved, before acceleration.
    Q_PROPERTY(qreal speed MEMBER m_speed NOTIFY speedChanged)
    // The layout's scale: item pixels per design pixel (1240 x 1080 at 1).
    Q_PROPERTY(qreal s MEMBER m_s NOTIFY sChanged)
    Q_PROPERTY(bool touching READ touching NOTIFY touchingChanged)

public:
    explicit TrackpadArea(QQuickItem *parent = nullptr);
    ~TrackpadArea() override;

    VirtualMouse *mouse() const { return m_mouse; }
    void setMouse(VirtualMouse *mouse);
    bool touching() const { return m_touching; }

    // Lift everything: no button left held.
    Q_INVOKABLE void reset();

signals:
    void mouseChanged();
    void speedChanged();
    void sChanged();
    void touchingChanged();
    // A mouse button went down: for haptics.
    void clicked();

protected:
    void touchEvent(QTouchEvent *event) override;
    void touchUngrabEvent() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    barry::TrackpadGesture *gesture();
    void frame(const std::map<int, barry::TrackpadGesture::Point> &live, double stampSeconds);
    void arm();

    QPointer<VirtualMouse> m_mouse;
    std::shared_ptr<barry::MouseCore> m_core;  // the one m_gesture moves
    std::unique_ptr<barry::TrackpadGesture> m_gesture;
    qreal m_speed = 1.6;
    qreal m_s = 1;
    bool m_touching = false;
    QTimer m_due;
};

class PressArea : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(VirtualMouse *mouse READ mouse WRITE setMouse NOTIFY targetChanged)
    Q_PROPERTY(DeckButtons *deck READ deck WRITE setDeck NOTIFY targetChanged)
    // mouse: "left", "right", "middle"; deck: "L4" ... "RightPadClick"
    Q_PROPERTY(QString button MEMBER m_button NOTIFY targetChanged)
    Q_PROPERTY(bool pressed READ pressed NOTIFY pressedChanged)

public:
    explicit PressArea(QQuickItem *parent = nullptr);
    ~PressArea() override;
    bool pressed() const { return m_finger != NONE; }
    VirtualMouse *mouse() const { return m_mouse; }
    void setMouse(VirtualMouse *mouse) { up(); m_mouse = mouse; emit targetChanged(); }
    DeckButtons *deck() const { return m_deck; }
    void setDeck(DeckButtons *deck) { up(); m_deck = deck; emit targetChanged(); }

signals:
    void targetChanged();
    void pressedChanged();

protected:
    void touchEvent(QTouchEvent *event) override;
    void touchUngrabEvent() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    static constexpr int NONE = -1000;
    void down(int finger);
    void up();

    QPointer<VirtualMouse> m_mouse;
    QPointer<DeckButtons> m_deck;
    QString m_button;
    int m_finger = NONE;
};

class DeckPadArea : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(DeckPad *pad READ pad WRITE setPad NOTIFY padChanged)
    // Where the finger is, fractions of the area (while touching).
    Q_PROPERTY(qreal fingerX READ fingerX NOTIFY fingerChanged)
    Q_PROPERTY(qreal fingerY READ fingerY NOTIFY fingerChanged)
    Q_PROPERTY(bool touching READ touching NOTIFY touchingChanged)

public:
    explicit DeckPadArea(QQuickItem *parent = nullptr);

    DeckPad *pad() const { return m_pad; }
    void setPad(DeckPad *pad);
    qreal fingerX() const { return m_x; }
    qreal fingerY() const { return m_y; }
    bool touching() const { return m_finger >= 0; }

signals:
    void padChanged();
    void fingerChanged();
    void touchingChanged();

protected:
    void touchEvent(QTouchEvent *event) override;
    void touchUngrabEvent() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    void follow(QPointF pos);
    void lift();

    QPointer<DeckPad> m_pad;
    int m_finger = -1;  // the point id followed
    qreal m_x = 0, m_y = 0;
};
