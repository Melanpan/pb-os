// KeyboardController: everything Barry's on-screen keyboards do, so the QML
// that draws them (KeyPanel.qml, and any skin) only lays out keys and
// reports touches.
//
// The keys: the keyboard pack's letter rows (KeyboardLanguages) with the
// number row, Shift, Backspace and the bottom row; a symbols page; an Fn
// page with a console's keys and sticky modifiers (Ctrl, Alt, Super: lit
// until the next key, which then goes out as a shortcut). Holding a text key
// offers its pack's alternatives; holding Backspace or an arrow repeats it;
// every key buzzes as it goes down (Haptics).
//
// Glide typing: a finger that starts on a letter and slides over the keys
// spells a word, which barry_launcher_shelld decodes (in the pack's words).
// The strip above the keys offers the next best words; backspace right
// after takes the whole word back. Autocorrect (autocorrect: true, the
// bottom screen's keyboard): the word just tapped goes to shelld with the
// space or punctuation after it, and a clear typo is swapped for the word
// meant, keys typed meanwhile waiting their turn; the strip then offers the
// word as typed, to undo it. While a word is typed the strip offers
// completions. A pack's composer (Composer.qml) sees every key first.
//
// Where the text goes: typer (Typer: XTest or KWin's input method), or,
// with preferKeyboard, keyboard (a uinput VirtualKeyboard, which games read)
// whenever the pack's letters are all on the US layout. Shortcuts go to
// keyboard when there is one.
#pragma once

#include "typer.h"
#include "virtualkeyboard.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QQmlParserStatus>
#include <QQuickItem>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <deque>
#include <functional>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

class KeyboardController : public QObject, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_ELEMENT

    // --- set by the keyboard's QML ---
    Q_PROPERTY(Typer *typer READ typer WRITE setTyper NOTIFY outputChanged)
    Q_PROPERTY(VirtualKeyboard *keyboard READ keyboard WRITE setKeyboard NOTIFY outputChanged)
    Q_PROPERTY(bool preferKeyboard READ preferKeyboard WRITE setPreferKeyboard NOTIFY outputChanged)
    Q_PROPERTY(bool hideKey READ hideKey WRITE setHideKey NOTIFY rowsChanged)
    Q_PROPERTY(bool glide READ glide WRITE setGlide NOTIFY languageChanged)
    Q_PROPERTY(bool autocorrect READ autocorrect WRITE setAutocorrect NOTIFY languageChanged)
    // Glide typing: the item the keys sit in (positions are its), and a
    // key's width there.
    Q_PROPERTY(QQuickItem *panel MEMBER m_panel NOTIFY panelChanged)
    Q_PROPERTY(qreal keyUnit MEMBER m_keyUnit NOTIFY panelChanged)

    // --- what to draw ---
    // The page's rows of keys: text keys (what they type unshifted) and
    // named ones (isNamed).
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(QString page READ page NOTIFY stateChanged)  // "letters", "symbols", "keys"
    Q_PROPERTY(bool shifted READ shifted NOTIFY stateChanged)
    Q_PROPERTY(bool ctrlOn READ ctrlOn NOTIFY stateChanged)
    Q_PROPERTY(bool altOn READ altOn NOTIFY stateChanged)
    Q_PROPERTY(bool superOn READ superOn NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap language READ language NOTIFY languageChanged)
    Q_PROPERTY(bool manyLanguages READ manyLanguages NOTIFY languageChanged)
    Q_PROPERTY(bool canGlide READ canGlide NOTIFY languageChanged)
    // The symbols page's non-ASCII row is navigation keys: its text could
    // not be typed (a uinput keyboard types US key positions).
    Q_PROPERTY(bool asciiOnly READ asciiOnly NOTIFY rowsChanged)
    // The strip: words to tap, and what they are ("glide", "complete",
    // "undo", "compose", or "" for none).
    Q_PROPERTY(QStringList alternatives READ alternatives NOTIFY stripChanged)
    Q_PROPERTY(QString stripMode READ stripMode NOTIFY stripChanged)
    // A glide: its path (in panel's coordinates), drawn while gliding.
    Q_PROPERTY(bool gliding READ gliding NOTIFY trailChanged)
    Q_PROPERTY(QVariantList trail READ trail NOTIFY trailChanged)
    // A held key's alternatives, while they show.
    Q_PROPERTY(QString moreFor READ moreFor NOTIFY moreChanged)
    Q_PROPERTY(QStringList moreAlts READ moreAlts NOTIFY moreChanged)

public:
    explicit KeyboardController(QObject *parent = nullptr);
    ~KeyboardController() override;

    void classBegin() override {}
    void componentComplete() override;

    Typer *typer() const { return m_typer; }
    void setTyper(Typer *typer);
    VirtualKeyboard *keyboard() const { return m_keyboard; }
    void setKeyboard(VirtualKeyboard *keyboard);
    bool preferKeyboard() const { return m_preferKeyboard; }
    void setPreferKeyboard(bool prefer);
    bool hideKey() const { return m_hideKey; }
    void setHideKey(bool hideKey);
    bool glide() const { return m_glide; }
    void setGlide(bool glide);
    bool autocorrect() const { return m_autocorrect; }
    void setAutocorrect(bool autocorrect);

    QVariantList rows() const { return m_rows; }
    QString page() const { return m_page; }
    bool shifted() const { return m_shifted; }
    bool ctrlOn() const { return m_ctrl; }
    bool altOn() const { return m_alt; }
    bool superOn() const { return m_super; }
    QVariantMap language() const { return m_language; }
    bool manyLanguages() const;
    bool canGlide() const;
    bool asciiOnly() const { return textToKeyboard(); }
    QStringList alternatives() const { return m_alternatives; }
    QString stripMode() const { return m_stripMode; }
    bool gliding() const { return m_gliding; }
    QVariantList trail() const;
    QString moreFor() const { return m_moreFor; }
    QStringList moreAlts() const { return m_moreAlts; }

    // --- the QML's reports ---
    Q_INVOKABLE void keyDown(const QString &key);  // a finger landed on it
    Q_INVOKABLE void keyUp(const QString &key);    // and left it
    Q_INVOKABLE void tap(const QString &key);
    Q_INVOKABLE void longPress(const QString &key);
    Q_INVOKABLE void pickMore(const QString &alternative);
    Q_INVOKABLE void dismissMore();
    Q_INVOKABLE void pickStrip(int index);
    Q_INVOKABLE void glideStart(qreal x, qreal y);
    Q_INVOKABLE void glideMove(qreal x, qreal y);
    Q_INVOKABLE void glideEnd();
    // A letter key's item, for glide typing (where it is).
    Q_INVOKABLE void registerKey(const QString &key, QQuickItem *item);
    Q_INVOKABLE void unregisterKey(const QString &key, QQuickItem *item);
    // Back to letters, nothing held or pending (the keyboard just opened).
    Q_INVOKABLE void reset();

    // --- for labels ---
    Q_INVOKABLE bool isNamed(const QString &key) const;
    // What a key types now (Shift's character while shifted).
    Q_INVOKABLE QString output(const QString &key) const;

signals:
    void outputChanged();
    void rowsChanged();
    void stateChanged();
    void languageChanged();
    void panelChanged();
    void stripChanged();
    void trailChanged();
    void moreChanged();
    void hideRequested();

private slots:
    void composerCandidates();

private:
    // keys
    void press(const QString &key);
    void sendCombo(const QStringList &keys);
    void undoCorrection(bool keepSep);
    void suggestFor(const QString &word);
    void pickAlternative(int index);
    void commitGlide(const QStringList &words);
    void clearStrip();
    void setStrip(const QStringList &alternatives, const QString &mode);
    void rebuildRows();
    void languageUpdated();
    bool isLetters(const QString &text) const;
    bool isDigit(const QString &text) const;
    bool isWordText(const QString &text) const;
    QString shiftOf(const QString &key) const;
    QString letterAt(QPointF pos) const;
    // the composer
    void loadComposer();
    QVariantMap callComposer(const char *method, const QVariant &arg, bool *handled);
    bool applyComposed(const QVariantMap &result);
    // output, in order
    bool textToKeyboard() const;
    void typed(const QString &text);
    void key(const QString &name);
    void replace(int back, const QString &text);
    void combo(const QStringList &keys);
    void plan(QJsonObject body, std::function<void(const QJsonObject *)> done);
    void drain();
    // shelld
    QNetworkReply *post(const QString &path, const QJsonObject &body);

    QPointer<Typer> m_typer;
    QPointer<VirtualKeyboard> m_keyboard;
    bool m_preferKeyboard = false;
    bool m_hideKey = true;
    bool m_glide = true;
    bool m_autocorrect = false;
    bool m_complete = false;
    QPointer<QQuickItem> m_panel;
    qreal m_keyUnit = 100;

    QVariantMap m_language;
    QHash<QString, QString> m_shiftMap;
    QVariantMap m_more;
    bool m_needsUnicode = false;  // letters off the US layout, or a composer
    QObject *m_composer = nullptr;

    QVariantList m_rows;
    QString m_page = QStringLiteral("letters");
    bool m_shifted = false, m_ctrl = false, m_alt = false, m_super = false;

    int m_serial = 0;        // counts presses: a late reply can tell it is stale
    QString m_word;          // the word being tapped
    struct Correction {
        QString from, to, sep;
    };
    std::optional<Correction> m_lastCorrection;  // until the next press
    QStringList m_alternatives;
    QString m_stripMode;

    QHash<QString, QPointer<QQuickItem>> m_keyItems;  // letter -> its key
    QList<QPointF> m_trail;
    QPointF m_glideFrom;
    bool m_glideOnLetter = false;
    bool m_gliding = false;     // this touch is a glide: its key does not type
    bool m_afterWord = false;   // the text ends in a word: a glide adds a space first
    QString m_lastGlide;        // what the last glide typed, its space included
    QString m_glideLead;

    QString m_moreFor;
    QStringList m_moreAlts;

    QTimer m_repeat;
    QString m_repeatKey;
    int m_repeatTicks = 0;

    // What to type, in order: a step waits behind one shelld is still
    // planning (autocorrect's swap for the word just typed).
    struct Step {
        std::function<void()> run;  // or, without: a plan
        QJsonObject plan;
        std::function<void(const QJsonObject *)> done;
    };
    std::deque<Step> m_steps;
    bool m_planning = false;
    QNetworkAccessManager *m_net = nullptr;
};
