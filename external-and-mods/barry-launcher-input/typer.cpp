#include "typer.h"

#include "virtualkeyboard.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
// barry_launcher_imd not there (yet): tried again this often.
constexpr int IM_RETRY_MS = 1000;
const QStringList KEYS = {"BackSpace", "Return", "Tab", "Left", "Right", "Up", "Down", "Escape",
                          "Delete", "space", "Home", "End", "Prior", "Next"};
}  // namespace

Typer::Typer(QObject *parent) : QObject(parent)
{
    m_imRetry.setInterval(IM_RETRY_MS);
    connect(&m_imRetry, &QTimer::timeout, this, &Typer::imConnect);
}

Typer::~Typer()
{
    imClose();
}

void Typer::setDesktop(bool desktop)
{
    if (desktop == m_desktop)
        return;
    m_desktop = desktop;
    emit desktopChanged();
    m_x.reset();
    if (desktop) {
        imConnect();
    } else {
        m_imRetry.stop();
        imClose();
    }
}

void Typer::setDisplay(const QString &display)
{
    if (display == m_display)
        return;
    m_display = display;
    emit displayChanged();
    m_x.reset();
}

void Typer::setKeyboard(VirtualKeyboard *keyboard)
{
    if (keyboard == m_keyboard)
        return;
    m_keyboard = keyboard;
    emit keyboardChanged();
}

// --- barry_launcher_imd ------------------------------------------------
// One line per message. It tells: {"event":"activate"|"deactivate"|...};
// it takes: COMMIT <text>, BACK <n>[ <text>], KEY <name>.

void Typer::imConnect()
{
    if (m_imFd >= 0 || !m_desktop)
        return;
    const QByteArray dir = qgetenv("XDG_RUNTIME_DIR");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s/barry_launcher_im.sock", dir.constData());
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || dir.isEmpty() || ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
        if (fd >= 0)
            close(fd);
        m_imRetry.start();
        return;
    }
    m_imRetry.stop();
    m_imFd = fd;
    m_imNotifier = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read);
    connect(m_imNotifier.get(), &QSocketNotifier::activated, this, &Typer::imRead);
}

void Typer::imClose()
{
    m_imNotifier.reset();
    if (m_imFd >= 0)
        close(m_imFd);
    m_imFd = -1;
    m_imLine.clear();
    setFieldActive(false);
}

void Typer::imRead()
{
    char buf[4096];
    const ssize_t n = ::read(m_imFd, buf, sizeof buf);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        imClose();
        if (m_desktop)
            m_imRetry.start();
        return;
    }
    m_imLine.append(buf, int(n));
    int nl;
    while ((nl = m_imLine.indexOf('\n')) >= 0) {
        const QByteArray line = m_imLine.left(nl);
        m_imLine.remove(0, nl + 1);
        const QString event = QJsonDocument::fromJson(line).object().value(QStringLiteral("event")).toString();
        if (event == QLatin1String("activate"))
            setFieldActive(true);
        else if (event == QLatin1String("deactivate"))
            setFieldActive(false);
    }
}

void Typer::setFieldActive(bool active)
{
    if (active == m_fieldActive)
        return;
    m_fieldActive = active;
    emit fieldActiveChanged();
}

bool Typer::imSend(const QString &line)
{
    QByteArray out = line.toUtf8();
    out.replace('\n', ' ');
    out.append('\n');
    for (qsizetype sent = 0; sent < out.size();) {
        const ssize_t n = ::send(m_imFd, out.constData() + sent, size_t(out.size() - sent), MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            imClose();
            m_imRetry.start();
            return false;
        }
        sent += n;
    }
    return true;
}

// --- typing ------------------------------------------------------------

barry::XTestTyper *Typer::x()
{
    if (!m_x)
        m_x = std::make_unique<barry::XTestTyper>(
            m_display.startsWith(QLatin1Char('$')) ? qgetenv(m_display.mid(1).toLocal8Bit().constData())
                                                   : m_display.toLocal8Bit());
    return m_x.get();
}

bool Typer::type(const QString &text)
{
    if (text.isEmpty())
        return true;
    if (im())
        return imSend(QStringLiteral("COMMIT ") + text);
    return x()->type(text);
}

bool Typer::key(const QString &name)
{
    if (!KEYS.contains(name))
        return false;
    if (im())
        return imSend(QStringLiteral("KEY ") + name);
    return x()->key(name);
}

bool Typer::replace(int back, const QString &text)
{
    back = qBound(0, back, 100);
    if (!back)
        return type(text);
    if (im())
        return imSend(text.isEmpty() ? QStringLiteral("BACK %1").arg(back)
                                     : QStringLiteral("BACK %1 %2").arg(QString::number(back), text));
    return x()->key(QStringLiteral("BackSpace"), back) && (text.isEmpty() || x()->type(text));
}

bool Typer::combo(const QStringList &keys)
{
    if (m_desktop && m_keyboard)
        return m_keyboard->combo(keys);
    return x()->combo(keys);
}
