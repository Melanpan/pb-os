#include "pbosqam.h"

#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

namespace {
QString socketPath()
{
    const QString env = qEnvironmentVariable("BARRY_PBOS_QAM_SOCKET");
    return env.isEmpty() ? QStringLiteral("/run/pbos-qam/barry.sock") : env;
}
}  // namespace

PbosQam *PbosQam::create(QQmlEngine *, QJSEngine *engine)
{
    return new PbosQam(engine);
}

PbosQam::PbosQam(QJSEngine *engine, QObject *parent) : QObject(parent), m_engine(engine), m_path(socketPath())
{
    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        for (const QByteArray &line : std::as_const(m_queue))
            m_socket.write(line);
        m_queue.clear();
        emit connectedChanged();
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        failAll(QStringLiteral("pbos-qamd went away"));
        emit connectedChanged();
    });
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        // Not there (pbos-qamd not running): the calls waiting fail; the
        // next call tries again.
        if (m_socket.state() != QLocalSocket::ConnectedState) {
            m_queue.clear();
            failAll(m_socket.errorString());
        }
    });
    connect(&m_socket, &QLocalSocket::readyRead, this, &PbosQam::readReplies);
}

void PbosQam::connectNow()
{
    if (m_socket.state() == QLocalSocket::UnconnectedState)
        m_socket.connectToServer(m_path);
}

void PbosQam::call(const QString &section, const QString &method, const QVariantList &args, const QJSValue &callback)
{
    const qint64 id = m_nextId++;
    QJsonObject req;
    req.insert(QStringLiteral("id"), id);
    req.insert(QStringLiteral("s"), section);
    req.insert(QStringLiteral("m"), method);
    req.insert(QStringLiteral("a"), QJsonArray::fromVariantList(args));
    const QByteArray line = QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n';
    m_pending.insert(id, callback);
    if (m_socket.state() == QLocalSocket::ConnectedState) {
        m_socket.write(line);
    } else {
        m_queue.append(line);
        connectNow();
    }
}

void PbosQam::readReplies()
{
    while (m_socket.canReadLine()) {
        const QJsonObject o = QJsonDocument::fromJson(m_socket.readLine()).object();
        const qint64 id = o.value(QLatin1String("id")).toInteger(-1);
        const auto it = m_pending.find(id);
        if (it == m_pending.end())
            continue;
        const QJSValue callback = it.value();
        m_pending.erase(it);
        const bool ok = o.value(QLatin1String("ok")).toBool();
        answer(callback, ok, ok ? o.value(QLatin1String("v")).toVariant() : o.value(QLatin1String("error")).toVariant());
    }
}

void PbosQam::failAll(const QString &why)
{
    const auto pending = std::exchange(m_pending, {});
    for (const QJSValue &callback : pending)
        answer(callback, false, why);
}

void PbosQam::answer(const QJSValue &callback, bool ok, const QVariant &value)
{
    if (!callback.isCallable())
        return;
    QJSValue cb = callback;
    const QJSValue result = cb.call({QJSValue(ok), m_engine->toScriptValue(value)});
    if (result.isError())
        qWarning("PbosQam: callback failed: %s", qPrintable(result.toString()));
}
