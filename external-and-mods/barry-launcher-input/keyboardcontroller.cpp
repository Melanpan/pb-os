#include "keyboardcontroller.h"

#include "glideengine.h"
#include "haptics.h"
#include "keyboardlanguages.h"

#include <QDebug>
#include <QJSValue>
#include <QJsonArray>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <cmath>

namespace {

const QSet<QString> NAMED = {"shift", "BackSpace", "space", "Return", "symbols", "keys", "abc", "hide",
                             "lang", "ctrl", "alt", "super", "Escape", "Tab", "Home", "End", "Delete",
                             "Left", "Right", "Up", "Down", "Prior", "Next", "copy", "paste", "cut",
                             "selectall", "undo", "redo", "numbers", "deleteword"};
const QSet<QString> REPEATING = {"BackSpace", "Left", "Right", "Up", "Down", "Delete"};
const QHash<QString, QStringList> SHORTCUTS = {
    {"copy", {"ctrl", "c"}}, {"paste", {"ctrl", "v"}}, {"cut", {"ctrl", "x"}},
    {"selectall", {"ctrl", "a"}}, {"undo", {"ctrl", "z"}}, {"redo", {"ctrl", "shift", "z"}},
    {"deleteword", {"ctrl", "BackSpace"}},
};
constexpr int REPEAT_MS = 90;
constexpr int REPEAT_AFTER = 4;  // ticks held before a key repeats
constexpr int SPACE_HOLD_MS = 350;   // the space bar held this long is a cursor
constexpr qreal CURSOR_STEP = 0.45;  // of a key's width: the cursor's slide, per step
constexpr qreal MIN_CURSOR_STEP = 18;  // px, however small the keys
// A backspace's look at the word before the caret (and the character after
// it): this long after it (the field has had it; held, Backspace repeats
// faster), this many characters (GlideEngine suggests for 40 at most).
constexpr int LOOK_BACK_MS = 150;
constexpr int LOOK_BACK = 41;

QVariantList rowsOf(std::initializer_list<QStringList> rows)
{
    QVariantList out;
    for (const QStringList &r : rows)
        out << QVariant(r);
    return out;
}

QString capitalized(const QString &w, bool cap)
{
    return cap && !w.isEmpty() ? w.left(1).toUpper() + w.mid(1) : w;
}

}  // namespace

KeyboardController::KeyboardController(QObject *parent) : QObject(parent)
{
    m_repeat.setInterval(REPEAT_MS);
    connect(&m_repeat, &QTimer::timeout, this, [this] {
        if (++m_repeatTicks > REPEAT_AFTER) {
            m_repeated = true;
            press(m_repeatKey);
        }
    });
    KeyboardLanguages *languages = KeyboardLanguages::instance();
    connect(languages, &KeyboardLanguages::currentChanged, this, &KeyboardController::languageUpdated);
    connect(languages, &KeyboardLanguages::languagesChanged, this, [this] {
        emit languageChanged();
        rebuildRows();
    });
    // A grid's labels follow Shift.
    connect(this, &KeyboardController::stateChanged, this, &KeyboardController::rebuildGrid);
    m_spaceHold.setSingleShot(true);
    m_spaceHold.setInterval(SPACE_HOLD_MS);
    connect(&m_spaceHold, &QTimer::timeout, this, &KeyboardController::spaceHeld);
    m_lookBack.setSingleShot(true);
    m_lookBack.setInterval(LOOK_BACK_MS);
    connect(&m_lookBack, &QTimer::timeout, this, &KeyboardController::lookBackNow);
}

KeyboardController::~KeyboardController()
{
    delete m_composer;
}

void KeyboardController::componentComplete()
{
    m_complete = true;
    languageUpdated();
}

// --- settings -------------------------------------------------------------

void KeyboardController::setTyper(Typer *typer)
{
    if (typer == m_typer)
        return;
    m_typer = typer;
    emit outputChanged();
}

void KeyboardController::setKeyboard(VirtualKeyboard *keyboard)
{
    if (keyboard == m_keyboard)
        return;
    m_keyboard = keyboard;
    languageUpdated();  // whether the pack's letters can go through it
    emit outputChanged();
}

void KeyboardController::setPreferKeyboard(bool prefer)
{
    if (prefer == m_preferKeyboard)
        return;
    m_preferKeyboard = prefer;
    emit outputChanged();
    rebuildRows();
}

void KeyboardController::setHideKey(bool hideKey)
{
    if (hideKey == m_hideKey)
        return;
    m_hideKey = hideKey;
    rebuildRows();
}

void KeyboardController::setGlide(bool glide)
{
    if (glide == m_glide)
        return;
    m_glide = glide;
    emit languageChanged();
}

void KeyboardController::setAutocorrect(bool autocorrect)
{
    if (autocorrect == m_autocorrect)
        return;
    m_autocorrect = autocorrect;
    emit languageChanged();
}

// --- the language -----------------------------------------------------------

void KeyboardController::languageUpdated()
{
    if (!m_complete)
        return;
    const QVariantMap language = KeyboardLanguages::instance()->current();
    const bool changed = language != m_language;
    m_language = language;
    m_shiftMap.clear();
    const QVariantList rows = language.value("rows").toList(), shiftRows = language.value("shiftRows").toList();
    for (int r = 0; r < rows.size() && r < shiftRows.size(); r++) {
        const QVariantList plain = rows[r].toList(), shifted = shiftRows[r].toList();
        for (int k = 0; k < plain.size() && k < shifted.size(); k++)
            m_shiftMap.insert(plain[k].toString(), shifted[k].toString());
    }
    m_more = language.value("more").toMap();
    // The pack's letters, the held keys' too, all typeable on uinput's US
    // layout? Else (or with a composer) text goes to the typer.
    QString all = language.value("letters").toString();
    for (const QVariant &alts : m_more)
        all += alts.toStringList().join(QString());
    m_needsUnicode = !language.value("composer").toString().isEmpty()
        || (m_keyboard && !m_keyboard->canType(all));
    if (changed) {
        loadComposer();
        reset();
    }
    emit languageChanged();
    emit outputChanged();
    rebuildRows();
}

bool KeyboardController::manyLanguages() const
{
    return KeyboardLanguages::instance()->languages().size() > 1;
}

bool KeyboardController::canGlide() const
{
    return m_glide && !m_language.value("words").toString().isEmpty() && !m_composer && !isGridPack();
}

bool KeyboardController::isNamed(const QString &key) const
{
    return NAMED.contains(key);
}

bool KeyboardController::isDigit(const QString &text) const
{
    return text.size() == 1 && text[0] >= QLatin1Char('0') && text[0] <= QLatin1Char('9');
}

bool KeyboardController::isLetters(const QString &text) const
{
    if (text.isEmpty())
        return false;
    const QString letters = m_language.value("letters").toString();
    for (const QChar c : text)
        if (!letters.contains(c) || (c >= QLatin1Char('0') && c <= QLatin1Char('9')))
            return false;
    return true;
}

bool KeyboardController::isWordText(const QString &text) const
{
    return isLetters(text) || isDigit(text)
        || (text.size() == 1 && m_language.value("wordChars").toString().contains(text));
}

QString KeyboardController::shiftOf(const QString &key) const
{
    if (m_shiftMap.contains(key))
        return m_shiftMap.value(key);
    if (key.size() == 1) {
        const QString up = key.toUpper();
        return up.size() == 1 ? up : key;
    }
    return key;
}

QString KeyboardController::output(const QString &key) const
{
    return !isNamed(key) && m_shifted ? shiftOf(key) : key;
}

// --- the rows -------------------------------------------------------------------

void KeyboardController::rebuildRows()
{
    QVariantList rows;
    if (m_page == QLatin1String("keys")) {
        // Fn: what a console wants, five rows like the others.
        rows = rowsOf({{"Escape", "Tab", "Home", "End", "Prior", "Next", "Delete"},
                       {"copy", "paste", "cut", "selectall", "undo", "redo"},
                       {"Left", "Up", "Down", "Right"},
                       {"ctrl", "alt", "super", "shift", "BackSpace"},
                       {"abc", "symbols", "space", "Return", "hide"}});
    } else if (m_page == QLatin1String("symbols")) {
        // The number row stays on the letters page, so this page takes the
        // rarer symbols; five rows like the letters, so the keyboard keeps
        // its height.
        rows = rowsOf({{"[", "]", "{", "}", "<", ">", "^", "`", "|", "\\"},
                       {"@", "#", "$", "%", "&", "-", "+", "(", ")", "/"},
                       asciiOnly() ? QStringList{"Escape", "Tab", "Home", "Left", "Up", "Down", "Right", "End", "Delete"}
                                   : QStringList{"€", "£", "¥", "°", "•", "…", "×", "÷", "¿", "¡"},
                       {"=", "*", "\"", "'", ":", ";", "!", "?", "BackSpace"},
                       {"symbols", "_", "space", "~", "Return", "hide"}});
    } else {
        // The pack's rows, between the number row (unless it has a 4th row
        // of its own) and the bottom row; Shift and Backspace around its last.
        const QVariantList pack = m_language.value("rows").toList();
        if (pack.size() != 4)
            rows << QVariant(QStringList{"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"});
        for (int i = 0; i < pack.size(); i++) {
            QStringList row = pack[i].toStringList();
            if (i == pack.size() - 1) {
                row.prepend(QStringLiteral("shift"));
                row.append(QStringLiteral("BackSpace"));
            }
            rows << QVariant(row);
        }
        QStringList bottom{"symbols", "keys"};
        if (manyLanguages())
            bottom << "lang";
        bottom << "," << "space" << "." << "Return" << "hide";
        rows << QVariant(bottom);
    }
    if (!m_hideKey) {
        for (QVariant &r : rows) {
            QStringList row = r.toStringList();
            row.removeAll(QStringLiteral("hide"));
            r = row;
        }
    }
    if (rows != m_rows) {
        m_rows = rows;
        emit rowsChanged();
    }
    rebuildGrid();
}

// --- touches ----------------------------------------------------------------------

void KeyboardController::keyDown(const QString &key)
{
    // A buzz as the key goes down, as a phone's keyboard does.
    Haptics::instance()->tick();
    if (key == QLatin1String("space")) {
        // Held, a cursor (spaceHeld).
        m_spaceSwallow = false;
        m_spaceHold.start();
    }
    if (REPEATING.contains(key)) {
        m_repeatKey = key;
        m_repeatTicks = 0;
        m_repeated = false;
        m_repeat.start();
    }
}

void KeyboardController::keyUp(const QString &key)
{
    if (key == m_repeatKey)
        m_repeat.stop();
    if (key == QLatin1String("space")) {
        m_spaceHold.stop();
        if (m_cursorMode) {
            // (Its tap may come after this.)
            m_spaceSwallow = true;
            endCursor();
        }
    }
}

void KeyboardController::tap(const QString &key)
{
    if (m_gliding)
        return;  // the glide this touch was types its word instead
    if (key == QLatin1String("space") && (m_cursorMode || m_spaceSwallow)) {
        m_spaceSwallow = false;
        return;  // it moved the cursor
    }
    press(output(key));
}

void KeyboardController::longPress(const QString &key)
{
    // Named keys: Backspace and the arrows repeat instead.
    if (isNamed(key) || m_gliding)
        return;
    const QString out = output(key);
    const QStringList alts = m_more.value(out).toStringList();
    if (alts.isEmpty()) {
        press(out);  // no alternatives: typed as a tap would
        return;
    }
    m_moreFor = out;
    m_moreAlts = alts;
    emit moreChanged();
}

void KeyboardController::pickMore(const QString &alternative)
{
    dismissMore();
    press(alternative);
}

void KeyboardController::dismissMore()
{
    if (m_moreFor.isEmpty())
        return;
    m_moreFor.clear();
    m_moreAlts.clear();
    emit moreChanged();
}

void KeyboardController::reset()
{
    m_shifted = m_ctrl = m_alt = m_super = false;
    m_page = QStringLiteral("letters");
    m_lastGlide.clear();
    m_afterWord = false;
    m_word.clear();
    m_lastCorrection.reset();
    m_repeat.stop();
    m_spaceHold.stop();
    m_lookBack.stop();
    m_spaceSwallow = false;
    endCursor();
    dismissMore();
    clearStrip();
    if (!m_touches.isEmpty()) {
        m_touches.clear();
        emit pressedChanged();
    }
    bool handled;
    if (m_composer)
        callComposer("reset", QVariant(), &handled);
    emit stateChanged();
    rebuildRows();
}

// --- keys -------------------------------------------------------------------------

void KeyboardController::sendCombo(const QStringList &keys)
{
    m_lastGlide.clear();
    m_lastCorrection.reset();
    m_word.clear();
    m_afterWord = false;
    clearStrip();
    combo(keys);
    m_ctrl = m_alt = m_super = m_shifted = false;
    emit stateChanged();
}

void KeyboardController::press(const QString &k)
{
    auto page = [this](const QString &p) {
        m_page = p;
        emit stateChanged();
        rebuildRows();
    };
    if (k == QLatin1String("shift")) { m_shifted = !m_shifted; emit stateChanged(); return; }
    if (k == QLatin1String("symbols")) {
        m_shifted = false;
        page(m_page == QLatin1String("symbols") ? QStringLiteral("letters") : QStringLiteral("symbols"));
        return;
    }
    if (k == QLatin1String("keys")) {
        page(m_page == QLatin1String("keys") ? QStringLiteral("letters") : QStringLiteral("keys"));
        return;
    }
    if (k == QLatin1String("numbers")) {
        m_shifted = false;
        page(m_page == QLatin1String("numbers") ? QStringLiteral("letters") : QStringLiteral("numbers"));
        return;
    }
    if (k == QLatin1String("abc")) { page(QStringLiteral("letters")); return; }
    if (k == QLatin1String("ctrl")) { m_ctrl = !m_ctrl; emit stateChanged(); return; }
    if (k == QLatin1String("alt")) { m_alt = !m_alt; emit stateChanged(); return; }
    if (k == QLatin1String("super")) { m_super = !m_super; emit stateChanged(); return; }
    if (k == QLatin1String("hide")) { emit hideRequested(); return; }
    if (k == QLatin1String("lang")) { KeyboardLanguages::instance()->next(); return; }
    dismissMore();
    m_serial++;
    // Shortcuts, and keys with a lit modifier, go out as combos.
    QStringList mods;
    if (m_ctrl) mods << "ctrl";
    if (m_alt) mods << "alt";
    if (m_super) mods << "super";
    if (SHORTCUTS.contains(k)) {
        const QStringList sc = SHORTCUTS.value(k);
        QStringList extra = mods;
        if (m_shifted)
            extra << "shift";
        QStringList keys;
        for (const QString &m : extra)
            if (!sc.contains(m))
                keys << m;
        sendCombo(keys + sc);
        return;
    }
    const bool named = isNamed(k) && k != QLatin1String("space");
    if (m_ctrl || m_alt || m_super || (m_shifted && named && k != QLatin1String("BackSpace"))) {
        const QString key = k == QLatin1String("space") ? k : named ? k : k.toLower();
        QStringList keys = mods;
        if (m_shifted)
            keys << "shift";
        sendCombo(keys << key);
        return;
    }
    if (k == QLatin1String("BackSpace") && !m_lastGlide.isEmpty()) {
        // Right after a glide: the whole word, and the space before it.
        replace(int(m_lastGlide.size()), QString());
        m_afterWord = !m_glideLead.isEmpty();
        m_lastGlide.clear();
        m_word.clear();
        clearStrip();
        lookBack();  // the word before it, if it ends there
        return;
    }
    if (k == QLatin1String("BackSpace") && m_lastCorrection) {
        // The word as typed, taken up again: its completions.
        undoCorrection(false);
        suggestFor(m_word);
        return;
    }
    const bool glided = !m_lastGlide.isEmpty();
    m_lastGlide.clear();
    m_lastCorrection.reset();
    if (m_stripMode != QLatin1String("compose"))
        clearStrip();
    if (m_composer) {
        // The composer first: it may change what was typed before.
        bool handled = false;
        const QVariantMap r = named ? callComposer("key", k, &handled)
                                    : callComposer("press", k == QLatin1String("space") ? QStringLiteral(" ") : k, &handled);
        if (handled && applyComposed(r)) {
            if (m_shifted) {
                m_shifted = false;
                emit stateChanged();
            }
            return;
        }
    }
    const bool correcting = this->correcting();
    if (named) {
        if (k == QLatin1String("BackSpace")) {
            key(k);
            if (m_word.size() > 1) {
                m_word.chop(1);
                suggestFor(m_word);
            } else {
                // Out of the word (or none known): what the field has
                // before the caret says whether a word ends there.
                m_word.clear();
                lookBack();
            }
            return;
        }
        const QString w = m_word;
        m_word.clear();
        m_afterWord = false;
        if (k == QLatin1String("Return") && correcting && w.size() >= 2)
            plan(QJsonObject{{"key", "Return"}, {"correct", w}}, nullptr);
        else
            key(k);
        return;
    }
    const QString text = k == QLatin1String("space") ? QStringLiteral(" ") : k;
    // A letter or digit tapped right after a glided word starts the next
    // word; punctuation sticks to it.
    const QString lead = glided && (isLetters(text) || isDigit(text)) ? QStringLiteral(" ") : QString();
    static const QRegularExpression ends(QStringLiteral("^[ ,.!?;:]$"));
    if (isWordText(text)) {
        m_word = (lead.isEmpty() ? m_word : QString()) + text;
        typed(lead + text);
        suggestFor(m_word);
    } else if (correcting && m_word.size() >= 2 && ends.match(text).hasMatch()) {
        const QString w = m_word;
        const int sent = m_serial;
        m_word.clear();
        QPointer<KeyboardController> self(this);
        plan(QJsonObject{{"text", text}, {"correct", w}}, [self, w, sent, text](const QJsonObject *reply) {
            // Only if nothing was typed meanwhile: undo acts on it.
            if (!self || !reply || self->m_serial != sent)
                return;
            const QString corrected = reply->value("corrected").toString();
            if (corrected.isEmpty())
                return;
            self->m_lastCorrection = Correction{w, corrected, text};
            self->setStrip({w}, QStringLiteral("undo"));
        });
    } else {
        m_word.clear();
        typed(lead + text);
    }
    m_afterWord = text != QLatin1String(" ");
    if (m_shifted) {
        m_shifted = false;
        emit stateChanged();
    }
}

void KeyboardController::undoCorrection(bool keepSep)
{
    const Correction c = *m_lastCorrection;
    m_lastCorrection.reset();
    clearStrip();
    plan(QJsonObject{{"back", int(c.to.size() + c.sep.size())}, {"text", c.from + (keepSep ? c.sep : QString())},
                     {"expect", c.to + c.sep}, {"keep", c.from}},
         nullptr);
    m_word = keepSep ? QString() : c.from;
    m_afterWord = true;
}

// --- the strip --------------------------------------------------------------------

void KeyboardController::clearStrip()
{
    setStrip({}, QString());
}

void KeyboardController::setStrip(const QStringList &alternatives, const QString &mode)
{
    if (alternatives == m_alternatives && mode == m_stripMode)
        return;
    m_alternatives = alternatives;
    m_stripMode = mode;
    emit stripChanged();
}

bool KeyboardController::correcting() const
{
    return m_autocorrect && !m_language.value("words").toString().isEmpty() && !m_composer;
}

void KeyboardController::suggestFor(const QString &w)
{
    if (!correcting() || w.size() < 2 || !isLetters(w)) {
        if (m_stripMode == QLatin1String("complete"))
            clearStrip();
        return;
    }
    GlideEngine::instance()->suggest(GlideEngine::Pack::of(m_language), w, this, [this, w](const GlideEngine::Words &words) {
        if (!words || m_word != w)
            return;  // typed on meanwhile
        setStrip(*words, words->isEmpty() ? QString() : QStringLiteral("complete"));
    });
}

void KeyboardController::pickStrip(int i)
{
    if (i < 0 || i >= m_alternatives.size())
        return;
    m_serial++;
    if (m_stripMode == QLatin1String("glide")) {
        pickAlternative(i);
    } else if (m_stripMode == QLatin1String("undo")) {
        undoCorrection(true);
    } else if (m_stripMode == QLatin1String("compose")) {
        bool handled = false;
        const QVariantMap r = callComposer("pick", i, &handled);
        if (handled)
            applyComposed(r);
    } else if (m_stripMode == QLatin1String("complete")) {
        const QString done = m_word;
        plan(QJsonObject{{"back", int(done.size())}, {"text", m_alternatives[i] + " "}, {"expect", done}}, nullptr);
        m_word.clear();
        m_afterWord = false;
        if (m_shifted) {
            m_shifted = false;
            emit stateChanged();
        }
        clearStrip();
    }
}

// --- the word before the caret -------------------------------------------------

void KeyboardController::lookBack()
{
    // A moment from now (lookBackNow), if nothing is pressed meanwhile.
    if (!correcting() || m_cursorMode)
        return;
    m_lookBackSerial = m_serial;
    m_lookBack.start();
}

void KeyboardController::lookBackNow()
{
    // What the field has before the caret, from keyd.
    if (m_serial != m_lookBackSerial || m_cursorMode)
        return;  // pressed on meanwhile
    if (m_planning || !m_steps.empty()) {
        m_lookBack.start();  // an autocorrect swap still going out: after it
        return;
    }
    const int sent = m_serial;
    GlideEngine::instance()->around(LOOK_BACK, 1, this, [this, sent](const GlideEngine::Around &text) {
        if (text && m_serial == sent && m_word.isEmpty() && m_lastGlide.isEmpty() && !m_cursorMode)
            lookedBack(text->before, text->after, LOOK_BACK);
    });
}

void KeyboardController::lookedBack(const QString &before, const QString &after, int asked)
{
    // The letters the field ends with, as a whole word: the word being
    // typed again, so a completion picked replaces all of it. Not with
    // more of it after the caret ("hel|lo"): a pick would leave that.
    const QString wordChars = m_language.value("wordChars").toString();
    auto inWord = [&](QChar ch) { return ch.isLetterOrNumber() || wordChars.contains(ch); };
    if (!after.isEmpty() && inWord(after[0]))
        return;
    qsizetype start = before.size();
    while (start > 0 && isLetters(before.mid(start - 1, 1)))
        start--;
    const QString w = before.mid(start);
    if (w.size() < 2)
        return;
    if (start == 0 && before.size() >= asked)
        return;  // maybe longer than what was asked
    if (start > 0 && inWord(before[start - 1]))
        return;  // the end of a word the pack cannot spell ("naïve", "don't")
    m_word = w;
    m_afterWord = true;
    suggestFor(w);
}

// --- the space bar as a cursor ----------------------------------------------------

void KeyboardController::spaceHeld()
{
    // Held long enough: no space; a grid's touch on it slides from here.
    if (m_gliding || m_cursorMode)
        return;
    startCursor();
    for (Touch &t : m_touches) {
        if (t.cell == QLatin1String("space") && !t.swiped) {
            t.swiped = true;
            t.start = t.at;
            t.slid = 0;
        }
    }
}

void KeyboardController::startCursor()
{
    m_spaceHold.stop();
    m_lookBack.stop();
    m_cursorMode = true;
    m_cursorAnchored = m_touching;
    m_cursorFrom = m_touchAt.x();
    m_cursorSlid = 0;
    // The word being typed is left where it is.
    m_serial++;
    m_word.clear();
    m_lastGlide.clear();
    m_lastCorrection.reset();
    m_afterWord = false;
    dismissMore();
    clearStrip();
    Haptics::instance()->tick();  // felt: a cursor now
    emit cursorModeChanged();
}

void KeyboardController::endCursor()
{
    if (!m_cursorMode)
        return;
    m_cursorMode = false;
    m_cursorAnchored = false;
    emit cursorModeChanged();
}

void KeyboardController::cursorTo(qreal x)
{
    if (!m_cursorAnchored) {
        m_cursorAnchored = true;
        m_cursorFrom = x;
        return;
    }
    const qreal step = std::max(MIN_CURSOR_STEP, CURSOR_STEP * m_keyUnit);
    const int steps = int((x - m_cursorFrom) / step);
    while (m_cursorSlid < steps) {
        m_cursorSlid++;
        cursorStep(QStringLiteral("Right"));
    }
    while (m_cursorSlid > steps) {
        m_cursorSlid--;
        cursorStep(QStringLiteral("Left"));
    }
}

void KeyboardController::cursorStep(const QString &name)
{
    // The arrow alone, whatever modifiers are lit, a buzz for each.
    m_serial++;
    key(name);
    Haptics::instance()->tick();
}

// --- glide typing -----------------------------------------------------------------

void KeyboardController::registerKey(const QString &key, QQuickItem *item)
{
    if (!isNamed(key) && key.size() == 1 && isLetters(key))
        m_keyItems.insert(key, item);
}

void KeyboardController::unregisterKey(const QString &key, QQuickItem *item)
{
    if (m_keyItems.value(key) == item)
        m_keyItems.remove(key);
}

QString KeyboardController::letterAt(QPointF pos) const
{
    for (auto it = m_keyItems.constBegin(); it != m_keyItems.constEnd(); ++it) {
        QQuickItem *item = it.value();
        if (item && m_panel && item->contains(item->mapFromItem(m_panel, pos)))
            return it.key();
    }
    return {};
}

QVariantList KeyboardController::trail() const
{
    QVariantList out;
    for (const QPointF &p : m_trail)
        out << p;
    return out;
}

void KeyboardController::glideStart(qreal x, qreal y)
{
    m_touching = true;
    m_touchAt = QPointF(x, y);
    if (m_cursorMode)
        return;  // another finger: the cursor's touch keeps it
    m_gliding = false;
    m_glideFrom = QPointF(x, y);
    m_trail = {m_glideFrom};
    m_glideOnLetter = canGlide() && m_page == QLatin1String("letters") && !(m_ctrl || m_alt || m_super)
        && !letterAt(m_glideFrom).isEmpty();
}

void KeyboardController::glideMove(qreal x, qreal y)
{
    m_touchAt = QPointF(x, y);
    if (m_cursorMode) {
        cursorTo(x);
        return;
    }
    if (!m_glideOnLetter)
        return;
    m_trail << QPointF(x, y);
    if (!m_gliding && std::hypot(x - m_glideFrom.x(), y - m_glideFrom.y()) > 0.6 * m_keyUnit)
        m_gliding = true;
    if (m_gliding)
        emit trailChanged();
}

void KeyboardController::glideEnd()
{
    m_touching = false;
    if (m_gliding && m_trail.size() > 1) {
        GlideEngine::Glide glide;
        glide.path = m_trail;
        glide.size = m_keyUnit;
        // The keys in the order the decoder always had them (a JSON
        // object's): it breaks ties by it.
        QJsonObject centers;
        for (auto it = m_keyItems.constBegin(); it != m_keyItems.constEnd(); ++it) {
            QQuickItem *item = it.value();
            if (!item || !m_panel)
                continue;
            const QPointF c = item->mapToItem(m_panel, QPointF(item->width() / 2, item->height() / 2));
            centers.insert(it.key(), QJsonArray{c.x(), c.y()});
        }
        for (auto it = centers.constBegin(); it != centers.constEnd(); ++it) {
            const QJsonArray c = it.value().toArray();
            glide.keys.append({it.key(), QPointF(c[0].toDouble(), c[1].toDouble())});
        }
        GlideEngine::instance()->decode(GlideEngine::Pack::of(m_language), glide, this, [this](const GlideEngine::Words &words) {
            if (words && !words->isEmpty())
                commitGlide(*words);
        });
    }
    m_trail.clear();
    emit trailChanged();
}

void KeyboardController::commitGlide(const QStringList &words)
{
    const bool cap = m_shifted;
    if (m_shifted) {
        m_shifted = false;
        emit stateChanged();
    }
    m_glideLead = m_afterWord ? QStringLiteral(" ") : QString();
    m_lastGlide = m_glideLead + capitalized(words[0], cap);
    typed(m_lastGlide);
    m_word.clear();
    m_lastCorrection.reset();
    QStringList alts;
    for (const QString &w : words.mid(1, 3))
        alts << capitalized(w, cap);
    setStrip(alts, QStringLiteral("glide"));
    m_afterWord = true;
}

void KeyboardController::pickAlternative(int i)
{
    const QString word = m_alternatives[i];
    const QString was = m_lastGlide.mid(m_glideLead.size());
    replace(int(m_lastGlide.size()), m_glideLead + word);
    m_lastGlide = m_glideLead + word;
    QStringList alts = m_alternatives;
    alts[i] = was;
    setStrip(alts, m_stripMode);
}

// --- the composer -----------------------------------------------------------------

void KeyboardController::loadComposer()
{
    if (m_composer) {
        delete m_composer;
        m_composer = nullptr;
    }
    const QString url = m_language.value("composer").toString();
    QQmlEngine *engine = qmlEngine(this);
    if (url.isEmpty() || !engine)
        return;
    QQmlComponent component(engine, QUrl(url));
    QObject *composer = component.create();
    if (!composer) {
        qWarning().noquote() << "Barry.Input: keyboard composer" << url << ":" << component.errorString();
        return;
    }
    m_composer = composer;
    // Its candidates, in the strip.
    const QMetaObject *mo = composer->metaObject();
    const int prop = mo->indexOfProperty("candidates");
    if (prop >= 0 && mo->property(prop).hasNotifySignal()) {
        const int slot = metaObject()->indexOfSlot("composerCandidates()");
        connect(composer, mo->property(prop).notifySignal(), this, metaObject()->method(slot));
    }
}

void KeyboardController::composerCandidates()
{
    if (!m_composer)
        return;
    QVariant v = m_composer->property("candidates");
    if (v.canConvert<QJSValue>())
        v = v.value<QJSValue>().toVariant();
    const QStringList candidates = v.toStringList();
    setStrip(candidates, candidates.isEmpty() ? QString() : QStringLiteral("compose"));
}

QVariantMap KeyboardController::callComposer(const char *method, const QVariant &arg, bool *handled)
{
    *handled = false;
    if (!m_composer)
        return {};
    const QMetaObject *mo = m_composer->metaObject();
    const bool withArg = arg.isValid();
    const QByteArray signature = QByteArray(method) + (withArg ? "(QVariant)" : "()");
    if (mo->indexOfMethod(signature.constData()) < 0)
        return {};  // the composer has no such function: the usual
    QVariant result;
    const bool ok = withArg ? QMetaObject::invokeMethod(m_composer, method, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, arg))
                            : QMetaObject::invokeMethod(m_composer, method, Q_RETURN_ARG(QVariant, result));
    if (!ok)
        return {};
    *handled = true;
    if (result.canConvert<QJSValue>())
        result = result.value<QJSValue>().toVariant();
    return result.toMap();
}

bool KeyboardController::applyComposed(const QVariantMap &r)
{
    // {back, text}: in place of the last back characters; nothing (null):
    // the key is the keyboard's.
    if (r.isEmpty())
        return false;
    replace(r.value("back").toInt(), r.value("text").toString());
    return true;
}

// --- output, in order ---------------------------------------------------------------

bool KeyboardController::textToKeyboard() const
{
    return m_preferKeyboard && m_keyboard && !m_needsUnicode;
}

void KeyboardController::typed(const QString &text)
{
    Step step;
    step.run = [this, text] {
        if (textToKeyboard())
            m_keyboard->type(text);
        else if (m_typer)
            m_typer->type(text);
    };
    m_steps.push_back(std::move(step));
    drain();
}

void KeyboardController::key(const QString &name)
{
    Step step;
    step.run = [this, name] {
        if (textToKeyboard())
            m_keyboard->key(name);
        else if (m_typer)
            m_typer->key(name);
    };
    m_steps.push_back(std::move(step));
    drain();
}

void KeyboardController::replace(int back, const QString &text)
{
    Step step;
    step.run = [this, back, text] {
        if (textToKeyboard())
            m_keyboard->replace(back, text);
        else if (m_typer)
            m_typer->replace(back, text);
    };
    m_steps.push_back(std::move(step));
    drain();
}

void KeyboardController::combo(const QStringList &keys)
{
    Step step;
    step.run = [this, keys] {
        // A real keyboard, where there is one: the focused window gets the
        // modifiers (and games read it).
        if (m_preferKeyboard && m_keyboard)
            m_keyboard->combo(keys);
        else if (m_typer)
            m_typer->combo(keys);
        else if (m_keyboard)
            m_keyboard->combo(keys);
    };
    m_steps.push_back(std::move(step));
    drain();
}

void KeyboardController::plan(const QJsonObject &body, std::function<void(const QJsonObject *)> done)
{
    Step step;
    step.plan = body;
    step.language = m_language;  // the pack it was typed in
    step.done = std::move(done);
    m_steps.push_back(std::move(step));
    drain();
}

void KeyboardController::drain()
{
    while (!m_planning && !m_steps.empty()) {
        Step step = std::move(m_steps.front());
        m_steps.pop_front();
        if (step.run) {
            step.run();
            continue;
        }
        // GlideEngine plans the swap; nothing typed meanwhile overtakes it.
        m_planning = true;
        auto done = std::move(step.done);
        GlideEngine::instance()->plan(GlideEngine::Pack::of(step.language), step.plan, this,
                                      [this, done](const GlideEngine::Plan &plan) {
            const QJsonObject &obj = plan.reply;
            const bool ok = plan.status == 200;
            if (ok) {
                // Backspaces, then text, then a key; nothing when the field
                // no longer ends as expected.
                const int back = obj.value("back").toInt();
                const QString text = obj.value("text").toString();
                const QString keyName = obj.value("key").toString();
                if (textToKeyboard()) {
                    m_keyboard->replace(back, text);
                    if (!keyName.isEmpty())
                        m_keyboard->key(keyName);
                } else if (m_typer) {
                    m_typer->replace(back, text);
                    if (!keyName.isEmpty())
                        m_typer->key(keyName);
                }
            }
            m_planning = false;
            if (done)
                done(ok ? &obj : nullptr);
            drain();
        });
    }
}
