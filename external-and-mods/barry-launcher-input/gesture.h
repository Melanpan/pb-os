// TrackpadGesture: the trackpad's gestures, on frames of finger positions,
// for both trackpads: TrackpadArea (the Trackpad app's pad, from Qt's
// touches) and TouchTrackpad (Desktop Mode, straight from the
// touchscreen). One finger moves the pointer (faster strokes go further), a
// tap clicks, a two-finger tap right-clicks, a three-finger tap
// middle-clicks, two fingers scroll, and a tap followed by a touch holds
// the button down to drag. Fingers on the Left and Right click regions hold
// those buttons; a tap on the dismiss region is reported.
//
// Positions are in design pixels (the 1240 x 1080 layout at s = 1), times
// in seconds. Not thread-safe: one caller at a time.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace barry {

class MouseCore;

class TrackpadGesture {
public:
    enum Region { None = -1, Pad, Left, Right, Dismiss };

    struct Point {
        double x, y;
    };

    struct Callbacks {
        std::function<void()> clicked;          // a mouse button went down
        std::function<void(int, int)> held;     // fingers on Left, Right
        std::function<void(int)> pressed;       // a press of Left (0) or Right (1)
        std::function<void()> dismissed;        // a tap on the dismiss region
        std::function<void(bool)> touching;     // a finger on the pad, or none
        std::function<double()> speed;          // pointer gain before acceleration
        std::function<Region(double, double)> region;  // where a finger came down
    };

    TrackpadGesture(std::shared_ptr<MouseCore> mouse, Callbacks callbacks);

    // The fingers down now, by id; stamp: when the touch hardware saw them.
    void frame(const std::map<int, Point> &live, double now, double stamp);
    // When a tap's click is due, if one is waiting.
    std::optional<double> nextDue() const { return m_clickAt; }
    void due(double now);
    // Lift everything: no button left held.
    void reset();
    // Fingers are down (as the last frame had them).
    bool active() const { return !m_contacts.empty(); }

private:
    struct Contact {
        int region;
        double x, y;
    };
    void down(int id, const Contact &c, double now);
    void move(const std::vector<std::pair<double, double>> &padMoves, double now, double stamp);
    void up(int id, const Contact &c, double now);
    void button(int code, bool down, bool up);

    std::shared_ptr<MouseCore> m_mouse;
    Callbacks m_cb;
    std::map<int, Contact> m_contacts;
    std::vector<int> m_pad;  // ids of the fingers on the pad
    int m_held[2] = {0, 0};  // fingers on Left, Right
    int m_fingers = 0;
    double m_moved = 0;
    double m_started = 0;
    double m_last = 0;      // the hardware's time of the last move
    double m_velocity = 0;  // finger speed, px/ms, smoothed
    bool m_dragging = false;
    bool m_dragMoved = false;
    double m_heldX = 0, m_heldY = 0;
    bool m_touching = false;
    std::optional<double> m_clickAt;
};

}  // namespace barry
