// PageSwipe: a page beside another, pulled in and pushed away sideways. On
// the home screen it is the performance dashboard to the left of home: a
// left-to-right swipe from anywhere pulls it in, right to left puts it away.
//
// It watches its children's touches (and mouse, for a test without a
// touchscreen) without taking them: a tap on a tile is still a tap. A touch
// becomes a swipe only once it has moved `threshold` pixels, mostly
// sideways, in a direction that moves the page; then the swipe takes the
// touch, and progress follows the finger. On release the page goes to the
// side it was thrown toward, or the nearer one, and settle() says where;
// the QML animates progress there (its look). A touch something keeps for
// itself (a slider with preventStealing) is never taken.
//
// progress: 0 = the page is away (home shows), 1 = the page is in.
#pragma once

#include <QElapsedTimer>
#include <QPointF>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

class QEventPoint;
class QPointerEvent;

class PageSwipe : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    // 0 (away) .. 1 (in); the QML's settling animation writes it too, and
    // may pass either end a little (an overshoot).
    Q_PROPERTY(qreal progress READ progress WRITE setProgress NOTIFY progressChanged)
    // A finger is moving the page.
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged)
    // Where the page is going or has gone: in (true) or away.
    Q_PROPERTY(bool open READ isOpen NOTIFY openChanged)
    // Pixels a touch must travel before it is a swipe.
    Q_PROPERTY(qreal threshold MEMBER m_threshold NOTIFY thresholdChanged)
    // Off: touches pass untouched (an app on top, say).
    Q_PROPERTY(bool swipeEnabled MEMBER m_swipeEnabled NOTIFY swipeEnabledChanged)

public:
    explicit PageSwipe(QQuickItem *parent = nullptr);

    qreal progress() const { return m_progress; }
    void setProgress(qreal progress);
    bool dragging() const { return m_dragging; }
    bool isOpen() const { return m_open; }

    // Send the page in or away (settle() follows).
    Q_INVOKABLE void openPage();
    Q_INVOKABLE void closePage();

signals:
    void progressChanged();
    void draggingChanged();
    void openChanged();
    void thresholdChanged();
    void swipeEnabledChanged();
    // A finger took the page (stop any settling animation).
    void dragStarted();
    // Animate progress to target (0 or 1); velocity in progress per second.
    void settle(qreal target, qreal velocity);

protected:
    bool childMouseEventFilter(QQuickItem *item, QEvent *event) override;
    void touchEvent(QTouchEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void touchUngrabEvent() override;
    void mouseUngrabEvent() override;

private:
    // Returns true when the event is the swipe's (the filter then eats it).
    bool handle(QPointerEvent *event);
    void begin(const QEventPoint &point);
    bool move(QPointerEvent *event, const QEventPoint &point);
    void end();
    void cancel();
    void setDragging(bool dragging);
    void setOpen(bool open);
    void settleTo(qreal target);

    qreal m_progress = 0;
    bool m_dragging = false;
    bool m_open = false;
    qreal m_threshold = 24;
    bool m_swipeEnabled = true;

    // The touch being watched (-1: none).
    int m_pointId = -1;
    QPointF m_start;           // scene position where it came down
    qreal m_startProgress = 0; // progress then
    QPointF m_last;
    QElapsedTimer m_clock;
    qint64 m_lastMs = 0;
    qreal m_velocity = 0;      // progress per second, smoothed
};
