// glide-engine-test: GlideEngine answering the cases run.sh also asks
// barry_launcher_shelld, through the keyboards' path: the pack as
// KeyboardLanguages finds it (BARRY_KEYBOARDS_DIRS), the work on the
// engine's workers, the answer posted back to this (the GUI) thread.
//
//   glide-engine-test [--now] < cases.jsonl > answers.jsonl
//
// Each case line: {"id", "lang", "op", ...}; op is glide (path, keys, size),
// suggest (prefix), type (body: a /type request), wait (ms: this thread's
// events, e.g. a personal words file's change, for that long). Each answer
// line: {"id", "result", "us"}: glide and suggest {"words"}, type {"status",
// "reply"}; us the microseconds from the ask to the answer. --now: on this
// thread, no worker (the decoders' own time).
#include "../../glideengine.h"
#include "../../keyboardlanguages.h"
#include "json.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <cstdio>
#include <iostream>
#include <string>

namespace bg = barryglide;

namespace {

GlideEngine::Pack packFor(const QString &lang)
{
    for (const QVariant &l : KeyboardLanguages::instance()->languages())
        if (l.toMap().value(QStringLiteral("id")).toString() == lang)
            return GlideEngine::Pack::of(l.toMap());
    GlideEngine::Pack none;
    none.id = lang;
    return none;
}

bool point(const bg::json::Value &v, QPointF &p)
{
    if (v.type != bg::json::Value::Array || v.arr.size() != 2 || !v.arr[0].isNumber() || !v.arr[1].isNumber())
        return false;
    p = QPointF(v.arr[0].num, v.arr[1].num);
    return true;
}

// Waits on this thread's events until the engine answers.
template <class T, class Ask>
T await(Ask ask)
{
    QEventLoop loop;
    QObject context;
    T out{};
    bool done = false;
    ask(&context, [&](const T &r) {
        out = r;
        done = true;
        loop.quit();
    });
    if (!done)
        loop.exec();
    return out;
}

// shelld's answer: its words, or its 400.
QJsonObject words(const GlideEngine::Words &w)
{
    if (!w)
        return {{"status", 400}};
    return {{"words", QJsonArray::fromStringList(*w)}};
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const bool now = app.arguments().contains(QStringLiteral("--now"));
    GlideEngine *engine = GlideEngine::instance();
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        bg::json::Value c;
        const QJsonObject q = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        if (!bg::json::parse(line, c) || q.isEmpty()) {
            std::fprintf(stderr, "unreadable line\n");
            continue;
        }
        const QString op = q.value(QStringLiteral("op")).toString();
        const GlideEngine::Pack pack = packFor(q.value(QStringLiteral("lang")).toString(QStringLiteral("en")));
        QJsonValue result = QStringLiteral("<bad case>");
        QElapsedTimer t;
        t.start();
        if (op == QLatin1String("glide")) {
            GlideEngine::Glide g;
            const bg::json::Value *path = c.get("path"), *keys = c.get("keys"), *size = c.get("size");
            bool ok = path && path->type == bg::json::Value::Array && keys && keys->type == bg::json::Value::Object
                && size && size->isNumber();
            for (size_t i = 0; ok && i < path->arr.size(); ++i) {
                QPointF p;
                ok = point(path->arr[i], p);
                g.path << p;
            }
            // In the case's order, as shelld iterates them.
            for (size_t i = 0; ok && i < keys->obj.size(); ++i) {
                QPointF p;
                ok = point(keys->obj[i].second, p);
                g.keys.append({QString::fromStdString(keys->obj[i].first), p});
            }
            if (ok) {
                g.size = size->num;
                t.restart();
                result = words(now ? engine->decodeNow(pack, g)
                                   : await<GlideEngine::Words>([&](QObject *ctx, auto done) { engine->decode(pack, g, ctx, done); }));
            }
        } else if (op == QLatin1String("suggest")) {
            const QString prefix = q.value(QStringLiteral("prefix")).toString();
            result = words(now ? engine->suggestNow(pack, prefix)
                               : await<GlideEngine::Words>([&](QObject *ctx, auto done) { engine->suggest(pack, prefix, ctx, done); }));
        } else if (op == QLatin1String("type")) {
            const QJsonObject body = q.value(QStringLiteral("body")).toObject();
            const GlideEngine::Plan p = now ? engine->planNow(pack, body)
                                            : await<GlideEngine::Plan>([&](QObject *ctx, auto done) { engine->plan(pack, body, ctx, done); });
            result = QJsonObject{{"status", p.status}, {"reply", p.reply}};
        } else if (op == QLatin1String("wait")) {
            QEventLoop loop;
            QTimer::singleShot(q.value(QStringLiteral("ms")).toInt(), &loop, &QEventLoop::quit);
            loop.exec();
            result = true;
        }
        const qint64 us = t.nsecsElapsed() / 1000;
        const QJsonObject out{{"id", q.value(QStringLiteral("id"))}, {"result", result}, {"us", us}};
        std::cout << QJsonDocument(out).toJson(QJsonDocument::Compact).toStdString() << '\n' << std::flush;
    }
    return 0;
}
