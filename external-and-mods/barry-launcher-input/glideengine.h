// GlideEngine: the keyboards' words (barryglide, glide/) in the keyboard's
// own process: glides decoded, the strip's suggestions and autocorrect's
// plans, as barry_launcher_shelld's /glide, /suggest and /type answer them,
// without a request to it.
//
// One per process, shared by its KeyboardControllers. The work runs off the
// GUI thread: glides and suggestions on one worker, in the order asked;
// plans on another (they may wait on barry_launcher_keyd for the field's
// text); each answer comes back on the GUI thread, and none for an asker
// that is gone. A pack's words load on its first use (a blob mapped
// from $XDG_CACHE_HOME/barry_launcher/glide, built there the first time),
// and again when its words.txt or keyboard.json changes; packs that build
// the same blob share it, and its decoder.
//
// Personal words (personal-words.txt, personal-words-<lang>.txt: words the
// user undid a correction of) are watched: what the other keyboard (each
// is a process) keeps is never corrected here either.
#pragma once

#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QPointF>
#include <QPointer>
#include <QStringList>
#include <QThreadPool>
#include <QVariantMap>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace barryglide {
class Blob;
class Corrector;
class Decoder;
}

class GlideEngine : public QObject {
    Q_OBJECT

public:
    // A pack, as KeyboardLanguages found it: its folder, words (a path, or
    // "") and whose words they are (its own id, or wordsFrom's).
    struct Pack {
        QString id, dir, words, wordsLang;
        static Pack of(const QVariantMap &language);
    };
    // A glide: its path, the letter keys' centers (in the order shelld got
    // them: a QJsonObject's) and a key's width, all in one item's coordinates.
    struct Glide {
        QList<QPointF> path;
        QList<QPair<QString, QPointF>> keys;
        qreal size = 0;
    };
    // A plan's answer: shelld's status (200, or 400 and 409 for nothing to
    // type) and reply.
    struct Plan {
        int status = 200;
        QJsonObject reply;
    };

    static GlideEngine *instance();
    ~GlideEngine() override;

    // done runs on the GUI thread (the engine's, as context is), unless
    // context is gone by then. Words: none (nullopt) for what shelld would
    // not answer (its 400: a path or prefix out of bounds); an empty list
    // for a pack without words.
    using Words = std::optional<QStringList>;
    void decode(const Pack &pack, const Glide &glide, QObject *context, std::function<void(Words)> done);
    void suggest(const Pack &pack, const QString &prefix, QObject *context, std::function<void(Words)> done);
    // shelld's /type: {back, text, key, expect, keep, correct}.
    void plan(const Pack &pack, const QJsonObject &body, QObject *context, std::function<void(Plan)> done);
    // The focused field's text around the caret: up to before characters
    // before it and after characters after it (keyd's AROUND), once the
    // plans asked before are out; none if keyd cannot say.
    struct Caret {
        QString before, after;
    };
    using Around = std::optional<Caret>;
    void around(int before, int after, QObject *context, std::function<void(Around)> done);

    // The same at once, on the calling thread (tests).
    Words decodeNow(const Pack &pack, const Glide &glide);
    Words suggestNow(const Pack &pack, const QString &prefix);
    Plan planNow(const Pack &pack, const QJsonObject &body);
    Around aroundNow(int before, int after);

private:
    explicit GlideEngine(QObject *parent = nullptr);
    struct Dict;
    // Call with m_lock held.
    Dict *dictionary(const Pack &pack);
    void watchPersonal();
    template <class T>
    void run(QThreadPool &pool, QObject *context, std::function<T()> work, std::function<void(T)> done);

    QMutex m_lock;  // the dictionaries
    std::map<QString, std::unique_ptr<Dict>> m_packs;  // by pack id
    std::map<std::pair<uint64_t, uint64_t>, std::weak_ptr<const barryglide::Blob>> m_blobs;  // by blob key
    std::map<std::pair<uint64_t, uint64_t>, std::weak_ptr<barryglide::Decoder>> m_decoders;
    std::map<QString, QString> m_failed;  // pack id -> the stamp its words would not load at

    QString m_data;        // shelld's DATA: where personal words are
    QString m_keydSocket;  // barry_launcher_keyd's: the text before the caret
    QFileSystemWatcher m_watcher;
    std::atomic<int> m_personalGen{0};  // counts personal-words changes

    QThreadPool m_words;  // glides and suggestions, in order
    QThreadPool m_plans;  // plans, in order
};
