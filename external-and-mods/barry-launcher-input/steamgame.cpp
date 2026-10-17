#include "steamgame.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
QString gamePath()
{
    const QString env = qEnvironmentVariable("BARRY_STEAM_GAME");
    return env.isEmpty() ? QStringLiteral("/run/pbos-qam/steam-game.json") : env;
}

QUrl image(const QJsonObject &o, const char *key)
{
    const QString path = o.value(QLatin1String(key)).toString();
    return path.isEmpty() || !QFileInfo::exists(path) ? QUrl() : QUrl::fromLocalFile(path);
}
}  // namespace

SteamGame *SteamGame::create(QQmlEngine *, QJSEngine *)
{
    return new SteamGame;
}

SteamGame::SteamGame(QObject *parent) : QObject(parent), m_path(gamePath())
{
    m_settle.setSingleShot(true);
    m_settle.setInterval(30);
    connect(&m_settle, &QTimer::timeout, this, [this] {
        rewatch();
        read();
    });
    // pbos-qamd replaces the file (a rename), so its folder is watched too;
    // until the folder exists, the one above it.
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_settle, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_settle, qOverload<>(&QTimer::start));
    rewatch();
    read();
}

void SteamGame::rewatch()
{
    QStringList want;
    QDir dir = QFileInfo(m_path).absoluteDir();
    while (!dir.exists() && !dir.isRoot())
        dir.setPath(QFileInfo(dir.absolutePath()).absolutePath());
    want << dir.absolutePath();
    if (QFileInfo::exists(m_path))
        want << m_path;
    const QStringList now = m_watcher.files() + m_watcher.directories();
    for (const QString &p : now)
        if (!want.contains(p))
            m_watcher.removePath(p);
    for (const QString &p : want)
        if (!now.contains(p))
            m_watcher.addPath(p);
}

void SteamGame::read()
{
    QFile f(m_path);
    QJsonObject o;
    if (f.open(QIODevice::ReadOnly))
        o = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonObject profile = o.value(QLatin1String("profile")).toObject();
    const QString appid = o.value(QLatin1String("appid")).toString();
    const QString name = o.value(QLatin1String("name")).toString();
    const QUrl hero = image(o, "hero"), logo = image(o, "logo");
    const bool running = o.value(QLatin1String("running")).toBool();
    const QString profileAppid = profile.value(QLatin1String("appid")).toString();
    const QString profileName = profile.value(QLatin1String("name")).toString();
    if (appid == m_appid && name == m_name && hero == m_hero && logo == m_logo && running == m_running &&
        profileAppid == m_profileAppid && profileName == m_profileName)
        return;
    m_appid = appid;
    m_name = name;
    m_hero = hero;
    m_logo = logo;
    m_running = running;
    m_profileAppid = profileAppid;
    m_profileName = profileName;
    emit changed();
}
