// KeyboardLanguages: the keyboard packs the on-screen keyboards can type
// in, and the one in use. A pack is a folder with a keyboard.json (its keys)
// and, if it likes, a words.txt (glide typing, suggestions, autocorrect) and
// a Composer.qml (languages that compose, or pick from candidates). The
// folder's name is the pack's id. A pack's keys are rows (a phone's
// keyboard) or a grid (Thumb-Key's: big keys you tap or swipe out of); a
// grid pack can borrow another pack's words (wordsFrom).
//
// Packs come from /usr/share/barry_launcher/keyboards (shipped) and
// ~/.local/share/barry_launcher/keyboards (the user's own), where a pack
// with a shipped one's id takes its place; BARRY_KEYBOARDS_DIRS (colon-
// separated, later ones winning) replaces both. Both are watched: a pack
// copied in, changed or removed is picked up at once. The one in use is
// kept in ~/.config/barry_launcher/keyboard.json, shared by every keyboard.
// See /usr/share/barry_launcher/keyboards/README.md for the format.
#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

class KeyboardLanguages : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    // Each: {id, name, label, dir, rows, shiftRows, more, letters,
    // wordChars, words (a path, or ""), composer (a URL, or ""), layout
    // ("rows", or "grid": Thumb-Key's keys), grid and numbers (a grid
    // pack's: 3 rows of 3 {center, <direction>: text}), wordsFrom}
    Q_PROPERTY(QVariantList languages READ languages NOTIFY languagesChanged)
    Q_PROPERTY(QString currentId READ currentId WRITE setCurrentId NOTIFY currentChanged)
    Q_PROPERTY(QVariantMap current READ current NOTIFY currentChanged)
    // Packs left out, and why ("de: rows must be 3 or 4 lists of keys").
    Q_PROPERTY(QStringList problems READ problems NOTIFY languagesChanged)

public:
    // One per process: QML's singleton and KeyboardController's.
    static KeyboardLanguages *instance();
    static KeyboardLanguages *create(QQmlEngine *, QJSEngine *);

    QVariantList languages() const { return m_languages; }
    QString currentId() const { return m_current.value(QStringLiteral("id")).toString(); }
    void setCurrentId(const QString &id);
    QVariantMap current() const { return m_current; }
    QStringList problems() const { return m_problems; }

    // The next pack, around.
    Q_INVOKABLE void next();
    Q_INVOKABLE void rescan();

signals:
    void languagesChanged();
    void currentChanged();

private:
    explicit KeyboardLanguages(QObject *parent = nullptr);
    QStringList dirs() const;
    QString configPath() const;
    QString savedId() const;
    void save(const QString &id);
    void pick(const QString &id);
    void watch();
    QVariantMap load(const QString &dir, const QString &id, QString *problem) const;

    QVariantList m_languages;
    QVariantMap m_current;
    QStringList m_problems;
    QFileSystemWatcher m_watcher;
    QTimer m_settle;  // a pack being copied in: one rescan once it is all there
};
