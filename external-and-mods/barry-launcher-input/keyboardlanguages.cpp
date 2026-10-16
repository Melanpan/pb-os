#include "keyboardlanguages.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>

namespace {

const char SYSTEM_DIR[] = "/usr/share/barry_launcher/keyboards";
constexpr int MAX_ROW = 14;   // keys in a row
constexpr int MAX_KEY = 8;    // characters a key types
constexpr int MAX_MORE = 10;  // long-press alternatives for a key
constexpr int SETTLE_MS = 300;
// Names KeyPanel gives its own keys: a pack's key may not be one.
const QSet<QString> RESERVED = {"shift", "BackSpace", "space", "Return", "symbols", "keys", "abc", "hide",
                                "lang", "ctrl", "alt", "super", "Escape", "Tab", "Home", "End", "Delete",
                                "Left", "Right", "Up", "Down", "Prior", "Next", "copy", "paste", "cut",
                                "selectall", "undo", "redo"};

// What Shift makes of a key: its upper case, when that is one character
// ("ß" stays "ß", not "SS").
QString shiftOf(const QString &key)
{
    const QString up = key.toUpper();
    return key.size() == 1 && up.size() != 1 ? key : up;
}

// The English pack, should no pack be readable at all: the keyboard always
// has keys.
QVariantMap builtIn()
{
    const QVariantList rows = {
        QVariantList{"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
        QVariantList{"a", "s", "d", "f", "g", "h", "j", "k", "l"},
        QVariantList{"z", "x", "c", "v", "b", "n", "m"},
    };
    QVariantList shiftRows;
    QString letters;
    for (const QVariant &row : rows) {
        QVariantList up;
        for (const QVariant &k : row.toList()) {
            up << shiftOf(k.toString());
            letters += k.toString() + shiftOf(k.toString());
        }
        shiftRows << QVariant(up);
    }
    return {{"id", "en"}, {"name", "English"}, {"label", "EN"}, {"dir", ""}, {"rows", rows},
            {"shiftRows", shiftRows}, {"more", QVariantMap()}, {"letters", letters},
            {"wordChars", "'"}, {"words", ""}, {"composer", ""}};
}

}  // namespace

KeyboardLanguages *KeyboardLanguages::instance()
{
    static KeyboardLanguages *languages = new KeyboardLanguages;  // the process's, to its end
    return languages;
}

KeyboardLanguages *KeyboardLanguages::create(QQmlEngine *, QJSEngine *engine)
{
    KeyboardLanguages *languages = instance();
    engine->setObjectOwnership(languages, QJSEngine::CppOwnership);
    return languages;
}

KeyboardLanguages::KeyboardLanguages(QObject *parent) : QObject(parent)
{
    m_settle.setSingleShot(true);
    m_settle.setInterval(SETTLE_MS);
    connect(&m_settle, &QTimer::timeout, this, &KeyboardLanguages::rescan);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_settle.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_settle.start(); });
    rescan();
}

QStringList KeyboardLanguages::dirs() const
{
    const QByteArray env = qgetenv("BARRY_KEYBOARDS_DIRS");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    return {QString::fromLatin1(SYSTEM_DIR),
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                + QStringLiteral("/barry_launcher/keyboards")};
}

QString KeyboardLanguages::configPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/barry_launcher/keyboard.json");
}

QString KeyboardLanguages::savedId() const
{
    QFile f(configPath());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("language")).toString();
}

void KeyboardLanguages::save(const QString &id)
{
    if (savedId() == id)
        return;
    QDir().mkpath(QFileInfo(configPath()).path());
    QSaveFile f(configPath());
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(QJsonDocument(QJsonObject{{"language", id}}).toJson());
    f.commit();
}

QVariantMap KeyboardLanguages::load(const QString &dir, const QString &id, QString *problem) const
{
    QFile f(dir + QStringLiteral("/keyboard.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        *problem = QStringLiteral("no keyboard.json");
        return {};
    }
    QJsonParseError err;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll(), &err).object();
    if (err.error != QJsonParseError::NoError) {
        *problem = QStringLiteral("keyboard.json: %1").arg(err.errorString());
        return {};
    }
    const QString name = o.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty()) {
        *problem = QStringLiteral("needs a name");
        return {};
    }

    // rows (and shiftRows, the same shape): 3 or 4 lists of keys, each the
    // text it types. A 4th row takes the number row's place.
    auto readRows = [&](const QJsonValue &v, QVariantList *out) -> bool {
        const QJsonArray rows = v.toArray();
        if (rows.size() < 3 || rows.size() > 4)
            return false;
        for (const QJsonValue &r : rows) {
            const QJsonArray keys = r.toArray();
            if (keys.isEmpty() || keys.size() > MAX_ROW)
                return false;
            QVariantList row;
            for (const QJsonValue &k : keys) {
                const QString key = k.toString();
                if (key.isEmpty() || key.size() > MAX_KEY || RESERVED.contains(key))
                    return false;
                row << key;
            }
            *out << QVariant(row);
        }
        return true;
    };
    QVariantList rows, shiftRows;
    if (!readRows(o.value(QStringLiteral("rows")), &rows)) {
        *problem = QStringLiteral("rows must be 3 or 4 lists of 1-%1 keys, each the text it types").arg(MAX_ROW);
        return {};
    }
    if (o.contains(QStringLiteral("shiftRows"))) {
        bool same = readRows(o.value(QStringLiteral("shiftRows")), &shiftRows) && shiftRows.size() == rows.size();
        for (int i = 0; same && i < rows.size(); i++)
            same = shiftRows[i].toList().size() == rows[i].toList().size();
        if (!same) {
            *problem = QStringLiteral("shiftRows must have the same rows and keys as rows");
            return {};
        }
    } else {
        for (const QVariant &row : rows) {
            QVariantList up;
            for (const QVariant &k : row.toList())
                up << shiftOf(k.toString());
            shiftRows << QVariant(up);
        }
    }

    // more: long-press alternatives, {"e": ["é", "è", "ê"]}; a shifted key
    // offers them in upper case unless it has its own.
    QVariantMap more;
    const QJsonObject moreObj = o.value(QStringLiteral("more")).toObject();
    for (auto it = moreObj.begin(); it != moreObj.end(); ++it) {
        QVariantList alts;
        for (const QJsonValue &a : it.value().toArray()) {
            const QString alt = a.toString();
            if (!alt.isEmpty() && alt.size() <= MAX_KEY && alts.size() < MAX_MORE)
                alts << alt;
        }
        if (!alts.isEmpty())
            more.insert(it.key(), alts);
    }
    for (int r = 0; r < rows.size(); r++) {
        const QVariantList plain = rows[r].toList(), shifted = shiftRows[r].toList();
        for (int k = 0; k < plain.size(); k++) {
            const QString up = shifted[k].toString();
            if (more.contains(plain[k].toString()) && !more.contains(up)) {
                QVariantList alts;
                for (const QVariant &a : more.value(plain[k].toString()).toList())
                    alts << shiftOf(a.toString());
                more.insert(up, alts);
            }
        }
    }

    // The letters: what a word is made of (with wordChars), and the keys
    // glide typing follows.
    QString letters;
    auto addLetters = [&](const QVariantList &rowList) {
        for (const QVariant &row : rowList)
            for (const QVariant &k : row.toList())
                for (const QChar c : k.toString())
                    if (c.isLetterOrNumber() || c.isMark())
                        letters += c;
    };
    addLetters(rows);
    addLetters(shiftRows);
    for (const QVariant &alts : more)
        for (const QVariant &a : alts.toList())
            for (const QChar c : a.toString())
                if (c.isLetterOrNumber() || c.isMark())
                    letters += c;

    const QString wordChars = o.contains(QStringLiteral("wordChars"))
        ? o.value(QStringLiteral("wordChars")).toString() : QStringLiteral("'");
    auto packFile = [&](const char *key) -> QString {
        const QString rel = o.value(QLatin1String(key)).toString();
        if (rel.isEmpty())
            return {};
        // Inside the pack's folder only.
        const QString path = QDir(dir).absoluteFilePath(rel);
        if (!QFileInfo(path).canonicalFilePath().startsWith(QFileInfo(dir).canonicalFilePath() + QLatin1Char('/'))
            || !QFileInfo::exists(path))
            return QStringLiteral("!") + rel;
        return path;
    };
    const QString words = packFile("words"), composer = packFile("composer");
    if (words.startsWith(QLatin1Char('!')) || composer.startsWith(QLatin1Char('!'))) {
        *problem = QStringLiteral("%1 is not a file in the pack's folder")
                       .arg((words.startsWith(QLatin1Char('!')) ? words : composer).mid(1));
        return {};
    }

    QString label = o.value(QStringLiteral("label")).toString().trimmed();
    if (label.isEmpty())
        label = id.left(3).toUpper();
    return {{"id", id}, {"name", name}, {"label", label.left(4)}, {"dir", dir}, {"rows", rows},
            {"shiftRows", shiftRows}, {"more", more}, {"letters", letters}, {"wordChars", wordChars},
            {"words", words}, {"composer", composer.isEmpty() ? QString() : QUrl::fromLocalFile(composer).toString()}};
}

void KeyboardLanguages::rescan()
{
    static const QRegularExpression validId(QStringLiteral("^[A-Za-z0-9_-]{1,32}$"));
    QMap<QString, QVariantMap> byId;  // sorted by id
    QMap<QString, QString> problemById;
    for (const QString &base : dirs()) {
        const QStringList ids = QDir(base).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &id : ids) {
            if (!validId.match(id).hasMatch())
                continue;
            QString problem;
            const QVariantMap pack = load(base + QLatin1Char('/') + id, id, &problem);
            if (pack.isEmpty()) {
                problemById.insert(id, QStringLiteral("%1 (%2): %3").arg(id, base, problem));
            } else {
                byId.insert(id, pack);  // a later folder's (the user's) wins
                problemById.remove(id);
            }
        }
    }
    QVariantList languages;
    for (const QVariantMap &pack : byId)
        languages << pack;
    if (languages.isEmpty())
        languages << builtIn();
    QStringList problems = problemById.values();
    for (const QString &p : problems)
        if (!m_problems.contains(p))  // once, not at every rescan
            qWarning().noquote() << "Barry.Input: keyboard pack left out:" << p;
    const bool changed = languages != m_languages || problems != m_problems;
    m_languages = languages;
    m_problems = problems;
    watch();
    if (changed)
        emit languagesChanged();
    pick(savedId());
}

void KeyboardLanguages::watch()
{
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());
    QStringList paths;
    for (const QString &base : dirs()) {
        // A user folder not made yet: its parent, so its making is seen.
        QString d = base;
        while (!QFileInfo(d).isDir() && d.contains(QLatin1Char('/')))
            d = QFileInfo(d).path();
        paths << d;
        for (const QString &id : QDir(base).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            paths << base + QLatin1Char('/') + id;
    }
    for (const QVariant &l : m_languages) {
        const QVariantMap pack = l.toMap();
        if (!pack.value(QStringLiteral("dir")).toString().isEmpty())
            paths << pack.value(QStringLiteral("dir")).toString() + QStringLiteral("/keyboard.json");
    }
    // The other keyboard's choice of language.
    QDir().mkpath(QFileInfo(configPath()).path());
    paths << QFileInfo(configPath()).path();
    paths.removeDuplicates();
    m_watcher.addPaths(paths);
}

void KeyboardLanguages::pick(const QString &id)
{
    // The one asked for; else English; else the first.
    QVariantMap chosen = m_languages.first().toMap();
    for (const QString &want : {id, QStringLiteral("en")}) {
        auto it = std::find_if(m_languages.begin(), m_languages.end(), [&](const QVariant &l) {
            return l.toMap().value(QStringLiteral("id")).toString() == want;
        });
        if (it != m_languages.end()) {
            chosen = it->toMap();
            break;
        }
    }
    if (chosen != m_current) {
        m_current = chosen;
        emit currentChanged();
    }
}

void KeyboardLanguages::setCurrentId(const QString &id)
{
    pick(id);
    save(currentId());
}

void KeyboardLanguages::next()
{
    int at = 0;
    for (int i = 0; i < m_languages.size(); i++)
        if (m_languages[i].toMap().value(QStringLiteral("id")).toString() == currentId())
            at = i;
    setCurrentId(m_languages[(at + 1) % m_languages.size()].toMap().value(QStringLiteral("id")).toString());
}
