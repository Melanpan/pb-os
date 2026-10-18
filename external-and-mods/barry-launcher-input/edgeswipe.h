// EdgeSwipe: a finger pulling a side page in from the bottom screen's left or
// right edge over an app, followed as it moves (Shell.qml's dashboard and
// quick settings, like a phone's side panels).
//
// While an app is in front its window, not Barry's, gets the touches; and
// Barry only comes forward once its picture of the app is ready (shelld). So
// this reads the bottom touchscreen itself, as shelld does (reading an evdev
// node takes nothing away from gamescope), on a thread of its own, while
// armed:
//   - a finger coming down within `edge` of a side: edgeTouched(side), for
//     shelld to take the app's picture already;
//   - that finger moving inward past `threshold`, mostly sideways:
//     started(side), and from then on `progress` (0 to 1: how far it has
//     pulled the page across the screen) follows it;
//   - lifted: released(side, progress, velocity), velocity inward in screen
//     pixels a second over the last moments, for Shell to open the page or
//     let it go back;
//   - a second finger, or the finger going back past the edge before it
//     started: cancelled().
// side: 1 from the left (the dashboard), -1 from the right (the quick
// settings), as Shell.qml's PageSwipe pages.
#pragma once

#include <QObject>
#include <QtQml/qqmlregistration.h>
#include <atomic>
#include <thread>

class EdgeSwipe : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool armed READ armed WRITE setArmed NOTIFY armedChanged)
    Q_PROPERTY(qreal edge MEMBER m_edge NOTIFY edgeChanged)
    Q_PROPERTY(qreal threshold MEMBER m_threshold NOTIFY thresholdChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(int side READ side NOTIFY activeChanged)
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)

public:
    explicit EdgeSwipe(QObject *parent = nullptr);
    ~EdgeSwipe() override;

    bool armed() const { return m_armed; }
    void setArmed(bool armed);
    bool active() const { return m_active; }
    int side() const { return m_side; }
    qreal progress() const { return m_progress; }

signals:
    void armedChanged();
    void edgeChanged();
    void thresholdChanged();
    void activeChanged();
    void progressChanged();
    void edgeTouched(int side);
    void started(int side);
    void released(int side, qreal progress, qreal velocity);
    void cancelled();

private:
    void read(int fd);
    // From the reader thread, queued to this one.
    void postTouched(int side);
    void postStarted(int side);
    void postProgress(double progress);
    void postReleased(int side, double progress, double velocity);
    void postCancelled();

    bool m_armed = false;
    std::atomic<bool> m_armedNow{false};
    std::atomic<double> m_edgeNow{45}, m_thresholdNow{24};
    qreal m_edge = 45, m_threshold = 24;
    bool m_active = false;
    int m_side = 0;
    qreal m_progress = 0;
    int m_stopFd = -1;
    std::thread m_reader;
};
