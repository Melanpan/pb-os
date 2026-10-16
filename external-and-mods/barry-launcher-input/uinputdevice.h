// One uinput device. Each emitEvents() is a single write() ending in its
// SYN_REPORT, so a frame's events reach the compositor together. Not
// thread-safe: its owner locks.
#pragma once

#include <QByteArray>
#include <QString>
#include <set>
#include <vector>

namespace barry {

struct Event {
    int type;
    int code;
    int value;
};

// An absolute axis a device reports, and its range.
struct Axis {
    int code;
    int minimum;
    int maximum;
};

class UinputDevice {
public:
    UinputDevice(const QByteArray &name, const std::vector<int> &keys, const std::vector<int> &rels,
                 const std::vector<Axis> &axes = {}, const std::vector<int> &props = {});
    ~UinputDevice();
    UinputDevice(const UinputDevice &) = delete;
    UinputDevice &operator=(const UinputDevice &) = delete;

    bool ok() const { return m_fd >= 0; }
    QString error() const { return m_error; }

    void emitEvents(const std::vector<Event> &events);
    void key(int code, bool down);
    void releaseAll();

private:
    int m_fd = -1;
    QString m_error;
    std::set<int> m_held;
};

}  // namespace barry
