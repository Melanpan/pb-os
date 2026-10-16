#include "uinputdevice.h"

#include <linux/uinput.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

namespace barry {

UinputDevice::UinputDevice(const QByteArray &name, const std::vector<int> &keys, const std::vector<int> &rels,
                           const std::vector<Axis> &axes, const std::vector<int> &props)
{
    // /dev/uinput is open to the user at the seat (Steam's udev rules tag it
    // uaccess).
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        m_error = QStringLiteral("cannot open /dev/uinput: %1").arg(strerror(errno));
        return;
    }
    bool ok = true;
    if (!keys.empty()) {
        ok = ok && ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0;
        for (int k : keys)
            ok = ok && ioctl(fd, UI_SET_KEYBIT, k) == 0;
    }
    if (!rels.empty()) {
        ok = ok && ioctl(fd, UI_SET_EVBIT, EV_REL) == 0;
        for (int r : rels)
            ok = ok && ioctl(fd, UI_SET_RELBIT, r) == 0;
    }
    if (!axes.empty()) {
        ok = ok && ioctl(fd, UI_SET_EVBIT, EV_ABS) == 0;
        for (const Axis &a : axes) {
            uinput_abs_setup abs{};
            abs.code = __u16(a.code);
            abs.absinfo.minimum = a.minimum;
            abs.absinfo.maximum = a.maximum;
            ok = ok && ioctl(fd, UI_SET_ABSBIT, a.code) == 0 && ioctl(fd, UI_ABS_SETUP, &abs) == 0;
        }
    }
    for (int p : props)
        ok = ok && ioctl(fd, UI_SET_PROPBIT, p) == 0;
    uinput_setup setup{};
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.version = 1;
    strncpy(setup.name, name.constData(), UINPUT_MAX_NAME_SIZE - 1);
    ok = ok && ioctl(fd, UI_DEV_SETUP, &setup) == 0 && ioctl(fd, UI_DEV_CREATE) == 0;
    if (!ok) {
        m_error = QStringLiteral("cannot create %1: %2").arg(QString::fromUtf8(name), strerror(errno));
        close(fd);
        return;
    }
    m_fd = fd;
}

UinputDevice::~UinputDevice()
{
    if (m_fd < 0)
        return;
    releaseAll();
    ioctl(m_fd, UI_DEV_DESTROY);
    close(m_fd);
}

void UinputDevice::emitEvents(const std::vector<Event> &events)
{
    if (m_fd < 0 || events.empty())
        return;
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    std::vector<input_event> out(events.size() + 1);
    for (size_t i = 0; i <= events.size(); i++) {
        input_event &ev = out[i];
        ev.input_event_sec = now.tv_sec;
        ev.input_event_usec = now.tv_nsec / 1000;
        if (i < events.size()) {
            ev.type = events[i].type;
            ev.code = events[i].code;
            ev.value = events[i].value;
        } else {
            ev.type = EV_SYN;
            ev.code = SYN_REPORT;
            ev.value = 0;
        }
    }
    const ssize_t size = ssize_t(out.size() * sizeof(input_event));
    // Rejected only when the kernel's queue is full; the events are dropped
    // then, as a real device's would be.
    (void)!write(m_fd, out.data(), size_t(size));
}

void UinputDevice::key(int code, bool down)
{
    if (down)
        m_held.insert(code);
    else
        m_held.erase(code);
    emitEvents({{EV_KEY, code, down ? 1 : 0}});
}

void UinputDevice::releaseAll()
{
    const std::set<int> held = m_held;
    for (int code : held)
        key(code, false);
}

}  // namespace barry
