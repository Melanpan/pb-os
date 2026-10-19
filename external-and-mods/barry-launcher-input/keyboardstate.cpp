#include "keyboardstate.h"

#include "shellstate.h"

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {
const char SHELLD[] = "http://127.0.0.1:47824";  // barry_launcher_shelld
}  // namespace

KeyboardState::KeyboardState(QObject *parent) : QObject(parent)
{
    ShellState *shell = ShellState::instance();
    connect(shell, &ShellState::keyboardChanged, this, [this] {
        if (m_bottom)
            takeBottom();
    });
    connect(shell, &ShellState::topKeyboardChanged, this, [this] {
        if (m_followTop)
            takeTop();
    });
}

KeyboardState::~KeyboardState()
{
    if (m_bottom)
        ShellState::instance()->unwatch(QStringLiteral("keyboard"));
    if (m_followTop)
        ShellState::instance()->unwatch(QStringLiteral("top-keyboard"));
}

void KeyboardState::setBottom(bool bottom)
{
    if (bottom == m_bottom)
        return;
    m_bottom = bottom;
    ShellState *shell = ShellState::instance();
    if (bottom) {
        // Not before the keys have a size (set right after, as QML makes
        // this): 0 would drop the apps back down for a moment.
        if (m_panelHeight > 0)
            shell->setKeyboardHeight(m_panelHeight);
        shell->watch(QStringLiteral("keyboard"));
        takeBottom();
    } else {
        shell->unwatch(QStringLiteral("keyboard"));
    }
    emit settingsChanged();
}

void KeyboardState::setFollowTop(bool follow)
{
    if (follow == m_followTop)
        return;
    m_followTop = follow;
    if (follow) {
        ShellState::instance()->watch(QStringLiteral("top-keyboard"));
        takeTop();
    } else {
        ShellState::instance()->unwatch(QStringLiteral("top-keyboard"));
    }
    emit settingsChanged();
}

void KeyboardState::setPanelHeight(int height)
{
    if (height == m_panelHeight)
        return;
    m_panelHeight = height;
    // Only the bottom keyboard's keys push the apps up.
    if (m_bottom)
        ShellState::instance()->setKeyboardHeight(height);
    emit settingsChanged();
}

void KeyboardState::takeBottom()
{
    const QJsonValue value = ShellState::instance()->keyboard();
    if (!value.isObject())
        return;
    const QJsonObject st = value.toObject();
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
}

void KeyboardState::takeTop()
{
    const QJsonValue value = ShellState::instance()->topKeyboard();
    if (!value.isObject())
        return;
    const QJsonObject st = value.toObject();
    const int serial = st.value("serial").toInt(-1);
    // Only asks made while open: the one that opened the app came as its
    // argument.
    if (m_topSerial >= 0 && serial != m_topSerial)
        emit topKeyboardAsked(st.value("visible").toBool());
    m_topSerial = serial;
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
