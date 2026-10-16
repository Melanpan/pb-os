#include "keyboardstate.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {
const char SHELLD[] = "http://127.0.0.1:47824";  // barry_launcher_shelld
constexpr int POLL_MS = 100;
}  // namespace

KeyboardState::KeyboardState(QObject *parent) : QObject(parent)
{
    m_poll.setInterval(POLL_MS);
    connect(&m_poll, &QTimer::timeout, this, &KeyboardState::poll);
}

void KeyboardState::setBottom(bool bottom)
{
    if (bottom == m_bottom)
        return;
    m_bottom = bottom;
    emit settingsChanged();
    update();
}

void KeyboardState::setFollowTop(bool follow)
{
    if (follow == m_followTop)
        return;
    m_followTop = follow;
    emit settingsChanged();
    update();
}

void KeyboardState::update()
{
    if (m_bottom || m_followTop)
        m_poll.start();
    else
        m_poll.stop();
    if (!m_net && (m_bottom || m_followTop))
        m_net = new QNetworkAccessManager(this);
}

void KeyboardState::poll()
{
    if (m_bottom && !m_bottomAsking) {
        m_bottomAsking = true;
        QNetworkRequest request(QUrl(QStringLiteral("%1/keyboard?h=%2").arg(QLatin1String(SHELLD)).arg(m_panelHeight)));
        request.setTransferTimeout(2000);
        QNetworkReply *reply = m_net->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            reply->deleteLater();
            m_bottomAsking = false;
            if (reply->error() != QNetworkReply::NoError)
                return;
            const QJsonObject st = QJsonDocument::fromJson(reply->readAll()).object();
            const bool visible = st.value("visible").toBool();
            const bool atTop = st.value("top").toBool();
            const QString curtain = st.value("curtain").toString();
            const bool opening = visible && !m_shown;
            if (visible == m_shown && atTop == m_atTop && curtain == m_curtain)
                return;
            m_shown = visible;
            m_atTop = atTop;
            m_curtain = curtain;
            if (opening && m_controller)
                m_controller->reset();
            emit stateChanged();
        });
    }
    if (m_followTop && !m_topAsking) {
        m_topAsking = true;
        QNetworkRequest request(QUrl(QStringLiteral("%1/top-keyboard").arg(QLatin1String(SHELLD))));
        request.setTransferTimeout(2000);
        QNetworkReply *reply = m_net->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            reply->deleteLater();
            m_topAsking = false;
            if (reply->error() != QNetworkReply::NoError)
                return;
            const QJsonObject st = QJsonDocument::fromJson(reply->readAll()).object();
            const int serial = st.value("serial").toInt(-1);
            // Only asks made while open: the one that opened the app came
            // as its argument.
            if (m_topSerial >= 0 && serial != m_topSerial)
                emit topKeyboardAsked(st.value("visible").toBool());
            m_topSerial = serial;
        });
    }
}

void KeyboardState::hide()
{
    if (!m_net)
        m_net = new QNetworkAccessManager(this);
    QNetworkRequest request(QUrl(QStringLiteral("%1/keyboard").arg(QLatin1String(SHELLD))));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = m_net->post(request, QByteArrayLiteral("{\"visible\": false}"));
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    if (m_shown) {
        m_shown = false;
        emit stateChanged();
    }
}
