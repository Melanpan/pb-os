// Barry's Deck controls: the Steam Deck's trackpads and back buttons, drawn
// on the bottom screen, for the virtual Steam Deck controller InputPlumber
// shows Steam.
//
// They are uinput devices InputPlumber takes as more sources of that
// controller (50-ayn_thor.yaml): each DeckPad a one-touch touchscreen it
// reads as the Deck's left or right trackpad (patch 0004's
// trackpad: left|right), DeckButtons the back buttons (BTN_TRIGGER_HAPPY5-8,
// read as R4 R5 L4 L5) and the trackpads' clicks (BTN_TRIGGER_HAPPY11/12,
// mapped by capability map barry_deck_buttons). A udev rule
// (72-barry-deck.rules) keeps compositors and Steam off them, so only
// InputPlumber sees them.
//
// The devices live while these objects do (and are complete).
#pragma once

#include "uinputdevice.h"

#include <QObject>
#include <QQmlParserStatus>
#include <QtQml/qqmlregistration.h>
#include <memory>

class DeckPad : public QObject, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_ELEMENT
    // "left" or "right"; the device is "Barry Deck Left Pad" or "... Right Pad".
    Q_PROPERTY(QString side READ side WRITE setSide NOTIFY sideChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool touching READ touching NOTIFY touchingChanged)

public:
    explicit DeckPad(QObject *parent = nullptr);
    ~DeckPad() override;

    QString side() const { return m_side; }
    void setSide(const QString &side);
    bool ready() const { return m_device != nullptr; }
    bool touching() const { return m_touching; }

    void classBegin() override {}
    void componentComplete() override;

    // A finger at x, y: fractions of the pad, 0 to 1 from its top left.
    Q_INVOKABLE void touch(qreal x, qreal y);
    // The finger lifted.
    Q_INVOKABLE void release();

signals:
    void sideChanged();
    void readyChanged();
    void touchingChanged();

private:
    void create();

    QString m_side = QStringLiteral("right");
    bool m_complete = false;
    bool m_touching = false;
    int m_trackingId = 0;
    std::unique_ptr<barry::UinputDevice> m_device;
};

class DeckButtons : public QObject, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

public:
    explicit DeckButtons(QObject *parent = nullptr);
    ~DeckButtons() override;

    bool ready() const { return m_device != nullptr; }

    void classBegin() override {}
    void componentComplete() override;

    // "L4", "L5", "R4", "R5", "LeftPadClick", "RightPadClick"
    Q_INVOKABLE bool press(const QString &button);
    Q_INVOKABLE bool release(const QString &button);
    Q_INVOKABLE void releaseAll();

signals:
    void readyChanged();

private:
    std::unique_ptr<barry::UinputDevice> m_device;
};
