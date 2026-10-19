// keyboard-controller-test: KeyboardController as the bottom keyboard drives
// it (Keyboard.qml: autocorrect on, KeyPanel.qml's reports), offscreen, its
// text going to a stub Typer (this file's: a field with a caret, no X) and
// its look at the text before the caret to a stub barry_launcher_keyd (this
// file's, on $XDG_RUNTIME_DIR, answering "BEFORE n", "AFTER n" and
// "AROUND b a" from that field, as barry_launcher_keyd does). The
// words are the repo's packs (BARRY_KEYBOARDS_DIRS: en, en-thumbkey).
//
//   keyboard-controller-test   (exit 0: all passed)
//
// Covers: a word taken up again by a backspace (its completions; one picked
// replaces it, an autocorrected word's too; none when keyd cannot say), the
// space bar held as a cursor (steps for a slide, no space; a short tap still
// a space), no glide from the space bar, the grid pack's space bar held.
#include "../../haptics.h"
#include "../../keyboardcontroller.h"
#include "../../keyboardlanguages.h"
#include "../../typer.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQuickItem>
#include <QTimer>
#include <cstdio>
#include <functional>

// --- the stub Typer: a field with a caret ------------------------------------------

namespace {

struct Field {
    QString text;
    int caret = 0;
    QStringList log;  // every call: "type:x", "key:Left", "replace:3:x", "combo:ctrl+c"
    void insert(const QString &t)
    {
        text.insert(caret, t);
        caret += int(t.size());
    }
    void back(int n)
    {
        n = std::min(n, caret);
        text.remove(caret - n, n);
        caret -= n;
    }
    void set(const QString &t)
    {
        text = t;
        caret = int(t.size());
        log.clear();
    }
};
Field field;

}  // namespace

barry::XTestTyper::~XTestTyper() = default;

Typer::Typer(QObject *parent) : QObject(parent) {}
Typer::~Typer() = default;
void Typer::setDisplay(const QString &display) { m_display = display; }
void Typer::setDesktop(bool desktop) { m_desktop = desktop; }
void Typer::setKeyboard(VirtualKeyboard *keyboard) { m_keyboard = keyboard; }

bool Typer::type(const QString &text)
{
    field.log << QStringLiteral("type:") + text;
    field.insert(text);
    return true;
}

bool Typer::key(const QString &name)
{
    field.log << QStringLiteral("key:") + name;
    if (name == QLatin1String("BackSpace"))
        field.back(1);
    else if (name == QLatin1String("Left"))
        field.caret = std::max(0, field.caret - 1);
    else if (name == QLatin1String("Right"))
        field.caret = std::min(int(field.text.size()), field.caret + 1);
    else if (name == QLatin1String("space"))
        field.insert(QStringLiteral(" "));
    return true;
}

bool Typer::replace(int back, const QString &text)
{
    field.log << QStringLiteral("replace:%1:%2").arg(back).arg(text);
    field.back(back);
    field.insert(text);
    return true;
}

bool Typer::combo(const QStringList &keys)
{
    field.log << QStringLiteral("combo:") + keys.join(QLatin1Char('+'));
    return true;
}

namespace {

// --- the stub keyd -------------------------------------------------------------------

bool keydAnswers = true;

void serveKeyd(QLocalServer &server)
{
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&server] {
        while (QLocalSocket *s = server.nextPendingConnection()) {
            QObject::connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
            QObject::connect(s, &QLocalSocket::readyRead, s, [s] {
                if (!s->canReadLine())
                    return;
                const QByteArray line = s->readLine().trimmed();
                QJsonObject reply{{"text", QJsonValue::Null}};
                const QString before = field.text.left(field.caret), after = field.text.mid(field.caret);
                const QList<QByteArray> words = line.split(' ');
                if (keydAnswers && words.size() == 2 && words[0] == "BEFORE") {
                    reply["text"] = before.right(words[1].toInt());
                } else if (keydAnswers && words.size() == 2 && words[0] == "AFTER") {
                    reply["text"] = after.left(words[1].toInt());
                } else if (keydAnswers && words.size() == 3 && words[0] == "AROUND") {
                    reply["text"] = before.right(words[1].toInt());
                    reply["after"] = after.left(words[2].toInt());
                }
                s->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
                s->flush();
                s->disconnectFromServer();
            });
        }
    });
}

// --- checks ---------------------------------------------------------------------------

int failures = 0;

void check(bool ok, const QString &what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok)
        failures++;
}

// This thread's events until cond holds (or ms pass); whether it held.
bool waitFor(const std::function<bool()> &cond, int ms = 3000)
{
    QElapsedTimer t;
    t.start();
    while (!cond() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return cond();
}

void settle(int ms)
{
    waitFor([] { return false; }, ms);
}

// A tap, as KeyPanel.qml reports it: down, tapped, up.
void tapKey(KeyboardController &c, const QString &k)
{
    c.keyDown(k);
    c.tap(k);
    c.keyUp(k);
}

void typeWord(KeyboardController &c, const QString &w)
{
    for (const QChar ch : w)
        tapKey(c, QString(ch));
}

int count(const QString &entry)
{
    return int(field.log.count(entry));
}

}  // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    const QString run = qEnvironmentVariable("XDG_RUNTIME_DIR");
    QDir().mkpath(run);
    const QString sock = run + QStringLiteral("/barry_launcher_keyd.sock");
    QLocalServer::removeServer(sock);
    QLocalServer keyd;
    if (!keyd.listen(sock)) {
        std::printf("FAIL stub keyd: %s\n", qPrintable(keyd.errorString()));
        return 1;
    }
    serveKeyd(keyd);

    KeyboardLanguages::instance()->setCurrentId(QStringLiteral("en"));
    Typer typer;
    KeyboardController c;
    c.setTyper(&typer);
    c.setAutocorrect(true);
    c.componentComplete();
    check(c.language().value("id").toString() == QLatin1String("en"), "English pack loaded");
    auto strip = [&c] { return c.stripMode() + QLatin1Char(':') + c.alternatives().join(QLatin1Char(',')); };

    // --- a word taken up again --------------------------------------------------------
    field.set({});
    typeWord(c, QStringLiteral("thin"));
    check(waitFor([&] { return c.stripMode() == QLatin1String("complete"); }), "completions while typing: " + strip());
    tapKey(c, QStringLiteral("space"));
    check(waitFor([&] { return field.text == QLatin1String("thin "); }), "space commits: \"" + field.text + "\"");
    check(c.alternatives().isEmpty(), "space clears the strip");
    tapKey(c, QStringLiteral("BackSpace"));
    check(waitFor([&] { return c.stripMode() == QLatin1String("complete") && !c.alternatives().isEmpty(); }),
          "backspace into the word: its completions again: " + strip());
    check(field.text == QLatin1String("thin"), "the space deleted: \"" + field.text + "\"");
    {
        const QStringList alts = c.alternatives();
        int i = int(alts.indexOf(QStringLiteral("thing")));
        if (i < 0)
            i = 0;
        const QString want = alts.value(i) + QStringLiteral(" ");
        c.pickStrip(i);
        check(waitFor([&] { return field.text == want; }), "a completion picked replaces the word: \"" + field.text + "\"");
        check(c.alternatives().isEmpty(), "and the strip clears");
    }
    // Further backspaces stay in the word; one into the space clears.
    field.set(QStringLiteral("hi thin"));
    tapKey(c, QStringLiteral("BackSpace"));  // "hi thi": a word again (keyd)
    check(waitFor([&] { return c.stripMode() == QLatin1String("complete"); }), "backspace inside a word: " + strip());
    tapKey(c, QStringLiteral("BackSpace"));  // "hi th"
    check(waitFor([&] { return c.stripMode() == QLatin1String("complete") && c.alternatives().join(",").startsWith("th"); }),
          "another one: the shorter word's: " + strip());
    tapKey(c, QStringLiteral("BackSpace"));  // "hi t"
    tapKey(c, QStringLiteral("BackSpace"));  // "hi "
    settle(400);
    check(c.alternatives().isEmpty() && field.text == QLatin1String("hi "), "backspace into the space clears: " + strip());
    tapKey(c, QStringLiteral("BackSpace"));  // "hi"
    check(waitFor([&] { return c.stripMode() == QLatin1String("complete"); }),
          "and the word before it comes back: " + strip());

    // An autocorrected word: backspace right after undoes it (as before),
    // with the word as typed in the strip.
    field.set({});
    c.reset();
    typeWord(c, QStringLiteral("teh"));
    tapKey(c, QStringLiteral("space"));
    const bool corrected = waitFor([&] { return c.stripMode() == QLatin1String("undo"); });
    check(corrected, "teh autocorrected: \"" + field.text + "\" " + strip());
    if (corrected) {
        tapKey(c, QStringLiteral("BackSpace"));
        check(waitFor([&] { return field.text == QLatin1String("teh"); }), "backspace undoes it: \"" + field.text + "\"");
        // Then on: a letter and back out of it, then into the word (another
        // typo: the undone one is a personal word now).
        field.set({});
        c.reset();
        typeWord(c, QStringLiteral("recieve"));
        tapKey(c, QStringLiteral("space"));
        check(waitFor([&] { return c.stripMode() == QLatin1String("undo"); }),
              "recieve autocorrected: \"" + field.text + "\" " + strip());
        const QString fixedTo = field.text.trimmed();
        tapKey(c, QStringLiteral("q"));
        tapKey(c, QStringLiteral("BackSpace"));
        tapKey(c, QStringLiteral("BackSpace"));
        check(waitFor([&] { return c.stripMode() == QLatin1String("complete") && field.text == fixedTo; }),
              "the corrected word taken up again: \"" + field.text + "\" " + strip());
        if (c.stripMode() == QLatin1String("complete")) {
            const QString want = c.alternatives().value(0) + QStringLiteral(" ");
            c.pickStrip(0);
            check(waitFor([&] { return field.text == want; }), "a completion picked replaces the corrected word: \"" + field.text + "\"");
        }
    }

    // keyd with no answer: no strip.
    field.set({});
    c.reset();
    keydAnswers = false;
    typeWord(c, QStringLiteral("thin"));
    tapKey(c, QStringLiteral("space"));
    waitFor([&] { return field.text == QLatin1String("thin "); });
    tapKey(c, QStringLiteral("BackSpace"));
    settle(500);
    check(c.alternatives().isEmpty() && field.text == QLatin1String("thin"), "keyd silent: no strip: " + strip());
    keydAnswers = true;

    // After the cursor moved: the word the caret now ends.
    field.set(QStringLiteral("think. about"));
    c.reset();
    field.caret = 6;  // "think.| about"
    tapKey(c, QStringLiteral("BackSpace"));  // "think| about"
    check(waitFor([&] { return c.stripMode() == QLatin1String("complete"); }), "after the cursor moved: " + strip());
    // Inside a word ("hel|lo", a backspace: "he|lo"): not taken up, a pick
    // would leave the rest.
    field.set(QStringLiteral("hello"));
    c.reset();
    field.caret = 3;
    tapKey(c, QStringLiteral("BackSpace"));
    settle(500);
    check(field.text == QLatin1String("helo") && c.alternatives().isEmpty(), "inside a word (he|lo): no strip: " + strip());
    // Punctuation after the caret ("thin|, ok"): taken up.
    field.set(QStringLiteral("thinx, ok"));
    c.reset();
    field.caret = 5;
    tapKey(c, QStringLiteral("BackSpace"));
    check(waitFor([&] { return c.stripMode() == QLatin1String("complete"); }), "a comma after the caret (thin|, ok): " + strip());

    // --- the space bar as a cursor ---------------------------------------------------------
    const qreal unit = 112;  // a key's width at 1240 px
    c.setProperty("keyUnit", unit);
    const qreal step = 0.45 * unit;
    field.set(QStringLiteral("abcdefghij"));
    c.reset();
    typeWord(c, QStringLiteral("th"));
    waitFor([&] { return c.stripMode() == QLatin1String("complete"); });
    field.log.clear();
    c.glideStart(600, 500);
    c.keyDown(QStringLiteral("space"));
    settle(200);
    check(!c.cursorMode(), "space held 200 ms: not yet a cursor");
    c.glideMove(600 + 0.3 * step, 500);
    check(waitFor([&] { return c.cursorMode(); }, 1000), "space held 350 ms: a cursor");
    check(c.alternatives().isEmpty(), "the strip clears in cursor mode");
    c.glideMove(600 + 0.3 * step + 3.2 * step, 505);
    check(count("key:Right") == 3, QStringLiteral("3.2 steps right: %1 Right").arg(count("key:Right")));
    c.glideMove(600 + 0.3 * step - 1.5 * step, 505);
    check(count("key:Left") == 4, QStringLiteral("back to -1.5 steps: %1 Left").arg(count("key:Left")));
    c.tap(QStringLiteral("space"));  // tapped before it went up
    c.keyUp(QStringLiteral("space"));
    c.glideEnd();
    check(!c.cursorMode(), "lifted: no cursor");
    check(count("type: ") == 0 && !field.text.contains(QLatin1Char(' ')), "lifted: no space typed: " + field.log.join(" "));
    // Up before its tap, then the tap.
    field.log.clear();
    c.glideStart(600, 500);
    c.keyDown(QStringLiteral("space"));
    waitFor([&] { return c.cursorMode(); }, 1000);
    c.glideMove(600 - 2.1 * step, 500);
    c.keyUp(QStringLiteral("space"));
    c.tap(QStringLiteral("space"));
    c.glideEnd();
    check(count("key:Left") == 2 && count("type: ") == 0, "up, then its tap: 2 Left, no space: " + field.log.join(" "));
    // Lifted off the key (no tap at all), then a short tap: a space.
    field.log.clear();
    c.glideStart(600, 500);
    c.keyDown(QStringLiteral("space"));
    waitFor([&] { return c.cursorMode(); }, 1000);
    c.keyUp(QStringLiteral("space"));
    c.glideEnd();
    c.glideStart(600, 500);
    c.keyDown(QStringLiteral("space"));
    settle(60);
    c.tap(QStringLiteral("space"));
    c.keyUp(QStringLiteral("space"));
    c.glideEnd();
    check(!c.cursorMode() && count("type: ") == 1, "a short tap types a space: " + field.log.join(" "));
    // Shift lit: the arrows still go alone.
    field.log.clear();
    tapKey(c, QStringLiteral("shift"));
    c.glideStart(600, 500);
    c.keyDown(QStringLiteral("space"));
    waitFor([&] { return c.cursorMode(); }, 1000);
    c.glideMove(600 + 1.2 * step, 500);
    c.keyUp(QStringLiteral("space"));
    c.glideEnd();
    check(count("key:Right") == 1 && field.log.filter("combo").isEmpty(), "with Shift lit: plain Right: " + field.log.join(" "));
    c.reset();

    // --- no glide from the space bar ------------------------------------------------------
    QQuickItem panel;
    panel.setSize(QSizeF(1240, 600));
    c.setProperty("panel", QVariant::fromValue(&panel));
    QList<QQuickItem *> keys;
    const QString row = QStringLiteral("qwertyuiop");
    for (int i = 0; i < row.size(); i++) {
        auto *k = new QQuickItem(&panel);
        k->setPosition(QPointF(12 + i * 122, 100));
        k->setSize(QSizeF(112, 92));
        c.registerKey(row.mid(i, 1), k);
        keys << k;
    }
    // From a letter: a glide (the control).
    c.glideStart(12 + 56, 146);
    for (int x = 68; x <= 600; x += 40)
        c.glideMove(x, 146);
    check(c.gliding(), "a slide from a letter glides");
    c.glideEnd();
    settle(300);
    // From the space bar (not a letter), over the letters: no glide.
    field.set({});
    c.reset();
    c.glideStart(600, 500);
    c.keyDown(QStringLiteral("space"));
    for (int y = 500; y >= 146; y -= 30)
        c.glideMove(600, y);
    for (int x = 600; x >= 68; x -= 40)
        c.glideMove(x, 146);
    check(!c.gliding(), "a slide from the space bar does not glide");
    c.tap(QStringLiteral("space"));
    c.keyUp(QStringLiteral("space"));
    c.glideEnd();
    settle(300);
    check(field.text == QLatin1String(" "), "and its tap types a space only: \"" + field.text + "\"");

    // --- the grid pack's space bar held -----------------------------------------------------
    KeyboardLanguages::instance()->setCurrentId(QStringLiteral("en-thumbkey"));
    if (c.gridPage()) {
        auto *space = new QQuickItem(&panel);
        space->setPosition(QPointF(300, 500));
        space->setSize(QSizeF(600, 92));
        c.registerCell(QStringLiteral("space"), space);
        const qreal col = 300;
        c.setProperty("keyUnit", col);
        const qreal gstep = col / 6;
        field.set(QStringLiteral("abcdefghij"));
        c.touchDown(1, 600, 540);
        settle(100);
        c.touchMove(1, 603, 540);
        check(waitFor([&] { return c.cursorMode(); }, 1000), "grid: space held: a cursor");
        c.touchMove(1, 603 - 2.5 * gstep, 540);
        c.touchUp(1, 603 - 2.5 * gstep, 540);
        check(count("key:Left") == 2 && count("type: ") == 0, "grid: 2 Left, no space: " + field.log.join(" "));
        field.log.clear();
        c.touchDown(2, 600, 540);
        c.touchUp(2, 602, 540);
        check(count("type: ") == 1, "grid: a tap types a space: " + field.log.join(" "));
    } else {
        check(false, "grid: en-thumbkey's grid page");
    }

    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
