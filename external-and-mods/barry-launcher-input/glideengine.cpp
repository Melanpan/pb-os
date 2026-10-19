#include "glideengine.h"

#include "glide.h"
#include "unicode.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QSet>
#include <QStandardPaths>
#include <cmath>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace bg = barryglide;

namespace {

// shelld's limits on what it is asked.
constexpr int MAX_PATH = 4000;   // a glide's points
constexpr int MAX_PREFIX = 40;   // a suggestion's prefix
constexpr int MAX_BACK = 100;    // a plan's backspaces
constexpr int KEYD_TIMEOUT_MS = 500;
// The keys a plan may end with (shelld's KEYS).
const QSet<QString> KEYS = {"BackSpace", "Return", "Tab", "Left", "Right", "Up", "Down", "Escape", "Delete",
                            "space", "Home", "End", "Prior", "Next"};

std::string utf8(const QString &s)
{
    return s.toStdString();
}

QStringList strings(const std::vector<std::string> &v)
{
    QStringList out;
    out.reserve(qsizetype(v.size()));
    for (const std::string &s : v)
        out << QString::fromStdString(s);
    return out;
}

// A file's modification time, as shelld's stamp has it; empty if it is gone.
QString mtime(const QString &path)
{
    struct stat st;
    if (::stat(QFile::encodeName(path).constData(), &st) != 0)
        return {};
    return QString::number(qint64(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec);
}

// str.isalnum() of one character: a letter (barry_glide's table), else a
// number (Qt's: Python's numerics but for a few CJK ideographs, letters anyway).
bool isAlnum(char32_t c)
{
    return bg::uni::isAlpha(c) || QChar::isNumber(c);
}

}  // namespace

struct GlideEngine::Dict {
    QString stamp;  // words path, its and keyboard.json's mtimes
    std::shared_ptr<const bg::Blob> blob;
    std::shared_ptr<bg::Decoder> decoder;
    bg::Layout layout;
    std::string personal;
    int personalGen = 0;
    std::unique_ptr<bg::Corrector> corrector;
    std::vector<std::pair<std::u32string, std::u32string>> fold;  // a held key's letter -> its key's
};

GlideEngine::Pack GlideEngine::Pack::of(const QVariantMap &language)
{
    // KeyboardLanguages fills words from wordsFrom's pack, and keeps
    // wordsFrom only for a pack without words of its own.
    Pack p;
    p.id = language.value(QStringLiteral("id")).toString();
    p.dir = language.value(QStringLiteral("dir")).toString();
    p.words = language.value(QStringLiteral("words")).toString();
    const QString from = language.value(QStringLiteral("wordsFrom")).toString();
    p.wordsLang = !from.isEmpty() && from != p.id && !p.words.isEmpty() ? from : p.id;
    return p;
}

GlideEngine *GlideEngine::instance()
{
    // The application's: its workers finish with it.
    static QPointer<GlideEngine> engine;
    if (!engine)
        engine = new GlideEngine(QCoreApplication::instance());
    return engine;
}

GlideEngine::GlideEngine(QObject *parent) : QObject(parent)
{
    m_words.setMaxThreadCount(1);
    m_plans.setMaxThreadCount(1);
    m_data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/barry_launcher");
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty())
        runtime = QStringLiteral("/run/user/%1").arg(::getuid());
    m_keydSocket = runtime + QStringLiteral("/barry_launcher_keyd.sock");
    // Personal words kept (or edited) elsewhere: reread at the next use.
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        m_personalGen++;
        watchPersonal();
    });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] {
        m_personalGen++;
        watchPersonal();
    });
    watchPersonal();
}

GlideEngine::~GlideEngine()
{
    m_words.waitForDone();
    m_plans.waitForDone();
}

void GlideEngine::watchPersonal()
{
    // The folder (or, not made yet, the nearest one there is), so a file
    // appearing is seen, and each personal words file, for what is added.
    QString d = m_data;
    while (!QFileInfo(d).isDir() && d.contains(QLatin1Char('/')))
        d = QFileInfo(d).path();
    QStringList paths{d};
    for (const QString &f : QDir(m_data).entryList({QStringLiteral("personal-words*.txt")}, QDir::Files))
        paths << m_data + QLatin1Char('/') + f;
    // Watched again: a file replaced (or removed and made anew) drops out.
    QStringList stale = m_watcher.files() + m_watcher.directories();
    for (const QString &p : paths)
        stale.removeAll(p);
    if (!stale.isEmpty())
        m_watcher.removePaths(stale);
    QStringList fresh;
    for (const QString &p : paths)
        if (!m_watcher.files().contains(p) && !m_watcher.directories().contains(p))
            fresh << p;
    if (!fresh.isEmpty())
        m_watcher.addPaths(fresh);
}

// --- a pack's dictionary --------------------------------------------------------

GlideEngine::Dict *GlideEngine::dictionary(const Pack &pack)
{
    // shelld's dictionary(): loaded on first use and again when its files
    // change; none for a pack without words.
    if (pack.words.isEmpty() || pack.dir.isEmpty())
        return nullptr;
    const QString wordsTime = mtime(pack.words), packTime = mtime(pack.dir + QStringLiteral("/keyboard.json"));
    if (wordsTime.isEmpty() || packTime.isEmpty())
        return nullptr;
    const QString stamp = pack.words + QLatin1Char('\n') + wordsTime + QLatin1Char('\n') + packTime
        + QLatin1Char('\n') + pack.wordsLang;
    auto it = m_packs.find(pack.id);
    if (it != m_packs.end() && it->second->stamp == stamp) {
        Dict *d = it->second.get();
        const int gen = m_personalGen;
        if (d->personalGen != gen) {
            d->corrector = std::make_unique<bg::Corrector>(d->blob, d->layout, d->personal);
            d->personalGen = gen;
        }
        return d;
    }
    if (auto failed = m_failed.find(pack.id); failed != m_failed.end() && failed->second == stamp)
        return nullptr;  // said once; tried again when a file changes

    // keyboard.json as shelld reads it (KeyboardLanguages' view of it is
    // the keys drawn, with shifted alternatives added).
    QFile f(pack.dir + QStringLiteral("/keyboard.json"));
    bg::json::Value v;
    std::optional<bg::Pack> read;
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray text = f.readAll();
        if (bg::json::parse(std::string_view(text.constData(), size_t(text.size())), v))
            read = bg::Pack::fromJson(v);
    }
    if (!read) {
        m_failed[pack.id] = stamp;
        return nullptr;
    }
    const bool english = pack.wordsLang == QLatin1String("en");
    auto d = std::make_unique<Dict>();
    d->stamp = stamp;
    d->layout = bg::Layout::fromPack(*read, english);
    d->fold = read->glideFold();
    std::string error;
    std::shared_ptr<const bg::Blob> blob = bg::Blob::load(utf8(pack.words), english, d->layout.blobFold(), {}, &error);
    if (!blob) {
        qWarning().noquote() << "Barry.Input: keyboard" << pack.id << ": cannot read" << pack.words << ":"
                             << QString::fromStdString(error);
        m_failed[pack.id] = stamp;
        return nullptr;
    }
    // Two packs with the same words (a language's second layout) share
    // its blob, mapped once, and its decoder.
    const std::pair<uint64_t, uint64_t> key{blob->header().key[0], blob->header().key[1]};
    if (auto shared = m_blobs[key].lock())
        blob = shared;
    else
        m_blobs[key] = blob;
    d->blob = blob;
    d->decoder = m_decoders[key].lock();
    if (!d->decoder) {
        d->decoder = std::make_shared<bg::Decoder>(blob);
        m_decoders[key] = d->decoder;
    }
    d->personal = utf8(m_data + (english ? QStringLiteral("/personal-words.txt")
                                         : QStringLiteral("/personal-words-%1.txt").arg(pack.wordsLang)));
    d->personalGen = m_personalGen;
    d->corrector = std::make_unique<bg::Corrector>(blob, d->layout, d->personal);
    m_failed.erase(pack.id);
    qInfo().noquote() << "Barry.Input: keyboard" << pack.id << ":" << blob->wordCount() << "words"
                      << (blob->mapped() ? "(mapped)" : "(in memory)");
    Dict *out = d.get();
    m_packs[pack.id] = std::move(d);  // a changed pack's old blob goes once nothing holds it
    return out;
}

// --- answers ------------------------------------------------------------------------

GlideEngine::Words GlideEngine::decodeNow(const Pack &pack, const Glide &glide)
{
    // shelld's glide(): its checks, then the decoder (no words: none).
    if (glide.path.size() < 2 || glide.path.size() > MAX_PATH || !(glide.size > 0))
        return std::nullopt;
    std::vector<bg::Point> path;
    path.reserve(size_t(glide.path.size()));
    for (const QPointF &p : glide.path)
        path.push_back({p.x(), p.y()});
    std::vector<bg::Decoder::Key> keys;
    keys.reserve(size_t(glide.keys.size()));
    for (const auto &k : glide.keys)
        keys.push_back({utf8(k.first), {k.second.x(), k.second.y()}});
    QMutexLocker locker(&m_lock);
    Dict *d = dictionary(pack);
    if (!d)
        return QStringList();
    return strings(d->decoder->decode(path, keys, double(glide.size), 4, d->fold));
}

GlideEngine::Words GlideEngine::suggestNow(const Pack &pack, const QString &prefix)
{
    if (prefix.toUcs4().size() > MAX_PREFIX)
        return std::nullopt;
    QMutexLocker locker(&m_lock);
    Dict *d = dictionary(pack);
    return d ? strings(d->corrector->suggest(utf8(prefix))) : QStringList();
}

namespace {

// barry_launcher_keyd's answer to a line ("BEFORE n", "AROUND b a"); none
// if it cannot be had.
std::optional<QJsonObject> askKeyd(const QString &socketPath, const QByteArray &ask)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return std::nullopt;
    const timeval tv{KEYD_TIMEOUT_MS / 1000, (KEYD_TIMEOUT_MS % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const QByteArray path = QFile::encodeName(socketPath);
    bool ok = size_t(path.size()) < sizeof addr.sun_path;
    if (ok) {
        std::memcpy(addr.sun_path, path.constData(), size_t(path.size()));
        ok = ::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof addr) == 0;
    }
    QByteArray data;
    if (ok) {
        ok = ::send(fd, ask.constData(), size_t(ask.size()), MSG_NOSIGNAL) == ask.size();
        while (ok && !data.endsWith('\n')) {
            char buf[4096];
            const ssize_t got = ::recv(fd, buf, sizeof buf, 0);
            if (got < 0)
                ok = false;
            else if (got == 0)
                break;
            else
                data.append(buf, got);
        }
    }
    ::close(fd);
    if (!ok)
        return std::nullopt;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError)
        return std::nullopt;
    return doc.object();
}

// Up to n characters before the caret in the focused field, from
// barry_launcher_keyd (shelld's text_before_caret); none if they cannot be
// read.
std::optional<std::u32string> textBeforeCaret(const QString &socketPath, int n)
{
    const auto reply = askKeyd(socketPath, "BEFORE " + QByteArray::number(n) + '\n');
    const QJsonValue text = reply ? reply->value(QStringLiteral("text")) : QJsonValue();
    if (!text.isString())
        return std::nullopt;
    const QList<uint> ucs = text.toString().toUcs4();
    return std::u32string(ucs.begin(), ucs.end());
}

}  // namespace

GlideEngine::Plan GlideEngine::planNow(const Pack &pack, const QJsonObject &body)
{
    // shelld's type_input(): what Keyboard.qml types, in the order its keys
    // came, autocorrect's swap included. "back": backspaces first; "expect":
    // only if the field's text before the caret ends with it; "correct": the
    // word just typed, swapped for the word meant if the field really ends
    // with it; "keep": a word the user undid a correction of.
    auto fail = [](int status, const char *error) {
        return Plan{status, QJsonObject{{"error", QString::fromLatin1(error)}}};
    };
    const QJsonValue backV = body.value(QStringLiteral("back"));
    int back = 0;
    if (!backV.isUndefined()) {
        const double b = backV.toDouble(-1);
        if (!backV.isDouble() || b != std::floor(b) || b < 0 || b > MAX_BACK)
            return fail(400, "back must be 0-100");
        back = int(b);
    }
    const QString text = body.value(QStringLiteral("text")).toString();
    const QString keyName = body.value(QStringLiteral("key")).toString();
    const bool key = KEYS.contains(keyName);
    if (!(back || !text.isEmpty() || key || body.contains(QStringLiteral("keep"))))
        return fail(400, "need text, a known key or back");
    // The text before the caret ends with word, as a whole word.
    auto endsWithWord = [this](const QString &w) {
        const QList<uint> ucs = w.toUcs4();
        const std::u32string word(ucs.begin(), ucs.end());
        const auto before = textBeforeCaret(m_keydSocket, int(word.size()) + 1);
        if (!before || before->size() < word.size() || before->compare(before->size() - word.size(), word.size(), word) != 0)
            return false;
        return before->size() == word.size() || !isAlnum((*before)[before->size() - word.size() - 1]);
    };
    const QString expect = body.value(QStringLiteral("expect")).toString();
    if (!expect.isEmpty() && !endsWithWord(expect))
        return fail(409, "the field does not end with the expected text");
    if (body.value(QStringLiteral("keep")).isString()) {
        const std::string kept = utf8(body.value(QStringLiteral("keep")).toString());
        QMutexLocker locker(&m_lock);
        if (Dict *d = dictionary(pack)) {
            d->corrector->keep(kept);
            // The language's other layouts keep the same list.
            for (auto &[id, other] : m_packs)
                if (other.get() != d && other->personal == d->personal)
                    other->corrector->addPersonal(kept);
        }
    }
    QJsonObject reply{{"ok", true}, {"back", back}, {"text", text}};
    if (key)
        reply.insert(QStringLiteral("key"), keyName);
    const QString word = body.value(QStringLiteral("correct")).toString();
    if (!word.isEmpty() && !back) {
        std::optional<std::string> fixed;
        {
            QMutexLocker locker(&m_lock);
            if (Dict *d = dictionary(pack))
                fixed = d->corrector->correct(utf8(word));
        }
        if (fixed && endsWithWord(word)) {
            const QString to = QString::fromStdString(*fixed);
            reply.insert(QStringLiteral("back"), int(word.toUcs4().size()));
            reply.insert(QStringLiteral("text"), to + text);
            reply.insert(QStringLiteral("corrected"), to);
            qInfo().noquote() << "Barry.Input: autocorrect:" << word << "->" << to;
        }
    }
    return Plan{200, reply};
}

GlideEngine::Around GlideEngine::aroundNow(int before, int after)
{
    // One ask for both (keyd's AROUND); a keyd without it answers nothing.
    const auto reply = askKeyd(m_keydSocket, "AROUND " + QByteArray::number(before) + ' ' + QByteArray::number(after) + '\n');
    if (!reply)
        return std::nullopt;
    const QJsonValue b = reply->value(QStringLiteral("text")), a = reply->value(QStringLiteral("after"));
    if (!b.isString() || !a.isString())
        return std::nullopt;
    return Caret{b.toString(), a.toString()};
}

// --- off the GUI thread -----------------------------------------------------------

template <class T>
void GlideEngine::run(QThreadPool &pool, QObject *context, std::function<T()> work, std::function<void(T)> done)
{
    // Back through the engine (it outlives every asker), on whose thread
    // the asker is looked for: none, no answer.
    QPointer<QObject> to(context);
    pool.start([this, to, work = std::move(work), done = std::move(done)] {
        T result = work();
        QMetaObject::invokeMethod(
            this, [to, done, result = std::move(result)] {
                if (to)
                    done(result);
            },
            Qt::QueuedConnection);
    });
}

void GlideEngine::decode(const Pack &pack, const Glide &glide, QObject *context, std::function<void(Words)> done)
{
    run<Words>(m_words, context, [this, pack, glide] { return decodeNow(pack, glide); }, std::move(done));
}

void GlideEngine::suggest(const Pack &pack, const QString &prefix, QObject *context, std::function<void(Words)> done)
{
    run<Words>(m_words, context, [this, pack, prefix] { return suggestNow(pack, prefix); }, std::move(done));
}

void GlideEngine::plan(const Pack &pack, const QJsonObject &body, QObject *context, std::function<void(Plan)> done)
{
    run<Plan>(m_plans, context, [this, pack, body] { return planNow(pack, body); }, std::move(done));
}

void GlideEngine::around(int before, int after, QObject *context, std::function<void(Around)> done)
{
    run<Around>(m_plans, context, [this, before, after] { return aroundNow(before, after); }, std::move(done));
}
