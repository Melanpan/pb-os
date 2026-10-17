// SteamGame: the game selected on the top screen, for the bottom screen's
// hero art. pbos-qamd (the PB-OS tab in Steam) follows Steam's focus (a
// game tile, a game's page, the game started) and writes what it found to
// a file: the game, and its hero and logo images on disk (the user's own
// art, Steam's, or SteamGridDB's). This watches that file (inotify through
// QFileSystemWatcher, nothing polled) and says what it holds.
//
// The file is /run/pbos-qam/steam-game.json, or BARRY_STEAM_GAME (tests):
// {"appid": "...", "name": "...", "hero": path, "logo": path,
//  "running": bool, "profile": {"appid", "name"}}
// "profile" is the game for the quick settings' game profile: the one
// running, else the one Steam played last.
#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

class SteamGame : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    // A game is known (pbos-qamd has told one).
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString appid READ appid NOTIFY changed)
    Q_PROPERTY(QString name READ name NOTIFY changed)
    // file: URLs; empty when there is no such image.
    Q_PROPERTY(QUrl hero READ hero NOTIFY changed)
    Q_PROPERTY(QUrl logo READ logo NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(QString profileAppid READ profileAppid NOTIFY changed)
    Q_PROPERTY(QString profileName READ profileName NOTIFY changed)

public:
    static SteamGame *create(QQmlEngine *, QJSEngine *);

    bool available() const { return !m_appid.isEmpty(); }
    QString appid() const { return m_appid; }
    QString name() const { return m_name; }
    QUrl hero() const { return m_hero; }
    QUrl logo() const { return m_logo; }
    bool running() const { return m_running; }
    QString profileAppid() const { return m_profileAppid; }
    QString profileName() const { return m_profileName; }

signals:
    void changed();

private:
    explicit SteamGame(QObject *parent = nullptr);
    void rewatch();
    void read();

    QString m_path;
    QFileSystemWatcher m_watcher;
    QTimer m_settle;  // one read for a burst of events
    QString m_appid, m_name, m_profileAppid, m_profileName;
    QUrl m_hero, m_logo;
    bool m_running = false;
};
