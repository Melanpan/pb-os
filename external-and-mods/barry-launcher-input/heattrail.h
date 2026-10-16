// HeatTrail: a trackpad that remembers where it was touched, like a
// thermochromic surface. Each finger warms the shape of its contact (the
// touchscreen's contact ellipse, or a fingertip's size when it reports
// none); a finger held still keeps warming, one moving leaves a streak; the
// heat spreads a little and cools away over a few seconds. Heat shows as
// colors, coolest first, fading into the pad at none.
//
// Only a look: it watches source's touches (and mouse, for a test without
// a touchscreen) through an event filter, so source still gets them all;
// it takes no input of its own. It runs only while something is warm.
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QPointF>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QSizeF>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QtQml/qqmlregistration.h>
#include <array>

class HeatTrail : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    // The item whose touches warm it (a TrackpadArea, a DeckPadArea).
    Q_PROPERTY(QQuickItem *source READ source WRITE setSource NOTIFY sourceChanged)
    // Colors from barely warm to hottest; fewer than two: nothing shows.
    Q_PROPERTY(QStringList colors READ colors WRITE setColors NOTIFY colorsChanged)
    // Corner radius to clip to (the pad's).
    Q_PROPERTY(qreal radius MEMBER m_radius NOTIFY radiusChanged)
    // Seconds for heat to cool to about a third.
    Q_PROPERTY(qreal fade MEMBER m_fade NOTIFY fadeChanged)
    // A fingertip's width in item pixels, for touches without a contact
    // size (the height is a little more).
    Q_PROPERTY(qreal fingerSize MEMBER m_fingerSize NOTIFY fingerSizeChanged)
    Q_PROPERTY(bool warm READ warm NOTIFY warmChanged)

public:
    explicit HeatTrail(QQuickItem *parent = nullptr);
    ~HeatTrail() override;

    QQuickItem *source() const { return m_source; }
    void setSource(QQuickItem *source);
    QStringList colors() const { return m_colors; }
    void setColors(const QStringList &colors);
    bool warm() const { return m_timer.isActive(); }

    // Cool everything at once.
    Q_INVOKABLE void clear();

    void paint(QPainter *painter) override;

signals:
    void sourceChanged();
    void colorsChanged();
    void radiusChanged();
    void fadeChanged();
    void fingerSizeChanged();
    void warmChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    struct Finger {
        QPointF at;     // item pixels
        QSizeF size;    // contact ellipse, item pixels
    };

    void touchAt(int id, QPointF at, QSizeF size, bool down);
    void lift(int id);
    void stamp(QPointF at, QSizeF size, float amount);
    void tick();
    void wake();
    void resizeGrid();
    void buildPalette();
    QSizeF contact(QSizeF reported) const;

    QPointer<QQuickItem> m_source;
    QStringList m_colors;
    qreal m_radius = 0;
    qreal m_fade = 2.2;
    qreal m_fingerSize = 70;

    // Heat, 0-1, on a coarse grid of CELL-pixel cells.
    int m_w = 0;
    int m_h = 0;
    QVector<float> m_heat;
    QVector<float> m_spare;
    QImage m_image;
    std::array<QRgb, 256> m_palette{};
    QHash<int, Finger> m_fingers;
    QTimer m_timer;
    QElapsedTimer m_clock;
};
