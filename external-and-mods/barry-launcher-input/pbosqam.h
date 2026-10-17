// PbosQam: Barry Launcher's way to the PB-OS tab's backend (pbos-qamd), for
// the quick settings page: charging, fan profile, the screens' brightness
// and game profiles are the PB-OS tab's settings, kept in one place.
// pbos-qamd listens on a local socket (/run/pbos-qam/barry.sock, or
// BARRY_PBOS_QAM_SOCKET) for the desktop user only, and answers the few
// methods it lets Barry call (daemon/barrylink.cpp).
//
// One JSON object a line each way: {"id", "s": section, "m": method, "a":
// [args]} out, {"id", "ok", "v": value, "error"} back.
//
// PbosQam.call("tools", "get_charge_control", [], function (ok, value) {...})
#pragma once

#include <QHash>
#include <QJSValue>
#include <QLocalSocket>
#include <QObject>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

class PbosQam : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    // Connected to pbos-qamd.
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)

public:
    static PbosQam *create(QQmlEngine *, QJSEngine *engine);

    bool connected() const { return m_socket.state() == QLocalSocket::ConnectedState; }

    // callback(ok, value): value is the method's answer, or the error text.
    Q_INVOKABLE void call(const QString &section, const QString &method, const QVariantList &args = {},
                          const QJSValue &callback = QJSValue());

signals:
    void connectedChanged();

private:
    explicit PbosQam(QJSEngine *engine, QObject *parent = nullptr);
    void connectNow();
    void readReplies();
    void failAll(const QString &why);
    void answer(const QJSValue &callback, bool ok, const QVariant &value);

    QJSEngine *m_engine;
    QLocalSocket m_socket;
    QString m_path;
    qint64 m_nextId = 1;
    QHash<qint64, QJSValue> m_pending;
    QList<QByteArray> m_queue;  // lines waiting for the connection
};
