#include "pageswipe.h"

#include <QMouseEvent>
#include <QPointerEvent>
#include <QTouchEvent>
#include <QtMath>

namespace {
// A swipe is mostly sideways: its horizontal travel at least this many
// times its vertical travel when it starts.
constexpr qreal kSideways = 1.5;
// Thrown at least this fast (page widths per second), it goes that way
// whatever the distance.
constexpr qreal kFling = 0.6;
}  // namespace

PageSwipe::PageSwipe(QQuickItem *parent) : QQuickItem(parent)
{
    setFiltersChildMouseEvents(true);
    setAcceptTouchEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton);
    m_clock.start();
}

void PageSwipe::setProgress(qreal progress)
{
    if (qFuzzyCompare(progress + 1, m_progress + 1))
        return;
    m_progress = progress;
    emit progressChanged();
}

void PageSwipe::setDragging(bool dragging)
{
    if (dragging == m_dragging)
        return;
    m_dragging = dragging;
    emit draggingChanged();
}

void PageSwipe::setPage(int page)
{
    if (page == m_page)
        return;
    m_page = page;
    emit pageChanged();
}

int PageSwipe::clampPage(int page) const
{
    return qBound(m_minPage, page, qMax(m_minPage, m_maxPage));
}

void PageSwipe::settleTo(int target)
{
    target = clampPage(target);
    setPage(target);
    emit settle(target, m_velocity);
}

void PageSwipe::goTo(int page)
{
    cancel();
    m_velocity = 0;
    settleTo(page);
}

void PageSwipe::begin(const QEventPoint &point)
{
    m_pointId = point.id();
    m_start = m_last = point.scenePosition();
    m_startProgress = m_progress;
    m_lastMs = m_clock.elapsed();
    m_velocity = 0;
}

bool PageSwipe::move(QPointerEvent *event, const QEventPoint &point)
{
    const QPointF pos = point.scenePosition();
    const qreal w = qMax<qreal>(1, width());
    const qint64 now = m_clock.elapsed();
    if (now > m_lastMs) {
        const qreal v = (pos.x() - m_last.x()) / w * 1000.0 / qreal(now - m_lastMs);
        m_velocity = 0.6 * v + 0.4 * m_velocity;
        m_lastMs = now;
    }
    m_last = pos;

    const qreal dx = pos.x() - m_start.x();
    if (!m_dragging) {
        const qreal dy = pos.y() - m_start.y();
        if (qAbs(dx) < m_threshold || qAbs(dx) < kSideways * qAbs(dy))
            return false;
        // Only a direction with a page to go to.
        const int from = clampPage(qRound(m_startProgress));
        const bool toward = dx > 0 ? from < m_maxPage : from > m_minPage;
        if (!toward) {
            m_pointId = -1;  // this touch is not a swipe, whatever it does next
            return false;
        }
        // Something that keeps its touch (a slider) keeps it.
        if (auto *grabber = qobject_cast<QQuickItem *>(event->exclusiveGrabber(point))) {
            if (grabber != this && (grabber->keepMouseGrab() || grabber->keepTouchGrab())) {
                m_pointId = -1;
                return false;
            }
        }
        event->setExclusiveGrabber(point, this);
        // Measure from here, so the page does not jump by the threshold.
        m_start = pos;
        m_startProgress = m_progress;
        setDragging(true);
        emit dragStarted();
        return true;
    }
    // One page either way from where the swipe began, within the pages.
    const int from = clampPage(qRound(m_startProgress));
    const qreal lo = qMax<qreal>(m_minPage, from - 1), hi = qMin<qreal>(m_maxPage, from + 1);
    setProgress(qBound<qreal>(lo, m_startProgress + (pos.x() - m_start.x()) / w, hi));
    return true;
}

void PageSwipe::end()
{
    const bool was = m_dragging;
    m_pointId = -1;
    if (!was)
        return;
    setDragging(false);
    const int from = clampPage(qRound(m_startProgress));
    int target;
    if (m_velocity > kFling)
        target = from + 1;
    else if (m_velocity < -kFling)
        target = from - 1;
    else
        target = qRound(m_progress);
    settleTo(qBound(from - 1, target, from + 1));
}

void PageSwipe::cancel()
{
    m_pointId = -1;
    if (m_dragging) {
        setDragging(false);
        settleTo(qRound(m_progress));
    }
}

bool PageSwipe::handle(QPointerEvent *event)
{
    if (!m_swipeEnabled || !isVisible() || !isEnabled()) {
        if (m_dragging)
            cancel();
        return false;
    }
    for (const QEventPoint &point : event->points()) {
        switch (point.state()) {
        case QEventPoint::Pressed:
            if (m_pointId < 0 && !m_dragging)
                begin(point);
            break;
        case QEventPoint::Updated:
            if (point.id() == m_pointId && move(event, point))
                return true;
            break;
        case QEventPoint::Released: {
            if (point.id() != m_pointId)
                break;
            const bool was = m_dragging;
            end();
            if (was)
                return true;
            break;
        }
        default:
            break;
        }
    }
    return m_dragging;
}

bool PageSwipe::childMouseEventFilter(QQuickItem *, QEvent *event)
{
    switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::MouseButtonPress:
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease: {
        auto *pe = static_cast<QPointerEvent *>(event);
        // Mouse events made from touches: the touch itself is handled.
        if (pe->isSinglePointEvent() && static_cast<QMouseEvent *>(event)->source() != Qt::MouseEventNotSynthesized)
            return m_dragging;
        return handle(pe);
    }
    case QEvent::TouchCancel:
        cancel();
        return false;
    default:
        return false;
    }
}

void PageSwipe::touchEvent(QTouchEvent *event)
{
    if (event->type() == QEvent::TouchCancel) {
        cancel();
        return;
    }
    handle(event);
    event->setAccepted(true);
}

void PageSwipe::mousePressEvent(QMouseEvent *event)
{
    handle(event);
    event->setAccepted(true);
}

void PageSwipe::mouseMoveEvent(QMouseEvent *event)
{
    handle(event);
}

void PageSwipe::mouseReleaseEvent(QMouseEvent *event)
{
    handle(event);
}

void PageSwipe::touchUngrabEvent()
{
    cancel();
}

void PageSwipe::mouseUngrabEvent()
{
    cancel();
}
