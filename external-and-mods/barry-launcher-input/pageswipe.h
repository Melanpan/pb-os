// PageSwipe: pages side by side, pulled in and pushed away sideways. In
// Barry Launcher they are the performance dashboard, the apps (home) and
// the quick settings, left to right: a left-to-right swipe from anywhere
// brings in the page to the left, right to left the one to the right.
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
// progress: which page shows, from minPage to maxPage. A left-to-right
// swipe raises it: the page at progress p is the one p places LEFT of page
// 0 (in Barry: 1 the dashboard, 0 home, -1 the quick settings). One swipe
// moves one page at most.
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
    // minPage .. maxPage; the QML's settling animation writes it too, and
    // may pass a page a little (an overshoot).
    Q_PROPERTY(qreal progress READ progress WRITE setProgress NOTIFY progressChanged)
    // A finger is moving the pages.
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged)
    // The page going or gone to.
    Q_PROPERTY(int page READ page NOTIFY pageChanged)
    Q_PROPERTY(int minPage MEMBER m_minPage NOTIFY rangeChanged)
    Q_PROPERTY(int maxPage MEMBER m_maxPage NOTIFY rangeChanged)
    // Pixels a touch must travel before it is a swipe.
    Q_PROPERTY(qreal threshold MEMBER m_threshold NOTIFY thresholdChanged)
    // Off: touches pass untouched (an app on top, say).
    Q_PROPERTY(bool swipeEnabled MEMBER m_swipeEnabled NOTIFY swipeEnabledChanged)

public:
    explicit PageSwipe(QQuickItem *parent = nullptr);

    qreal progress() const { return m_progress; }
    void setProgress(qreal progress);
    bool dragging() const { return m_dragging; }
    int page() const { return m_page; }

    // Go to a page (settle() follows).
    Q_INVOKABLE void goTo(int page);

signals:
    void progressChanged();
    void draggingChanged();
    void pageChanged();
    void rangeChanged();
    void thresholdChanged();
    void swipeEnabledChanged();
    // A finger took the page (stop any settling animation).
    void dragStarted();
    // Animate progress to target (a page); velocity in progress per second.
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
    void setPage(int page);
    void settleTo(int target);
    int clampPage(int page) const;

    qreal m_progress = 0;
    bool m_dragging = false;
    int m_page = 0;
    int m_minPage = 0;
    int m_maxPage = 1;
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
