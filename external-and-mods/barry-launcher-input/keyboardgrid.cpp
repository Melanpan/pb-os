// KeyboardController's grid packs: Thumb-Key's layout (github.com/dessalines/
// thumb-key), big keys, each a letter to tap and up to eight more to swipe
// out of it. GridPanel.qml draws m_grid and reports every touch; this finds
// the key it went down on and, when it lifts, what it typed: the key's
// center for a tap, else the swipe nearest the way the finger went. Also:
// sliding along the space bar (or holding it, then sliding) moves the
// cursor, a swipe left on Backspace deletes the word before it.
#include "keyboardcontroller.h"

#include "haptics.h"

#include <QtMath>
#include <cmath>

namespace {

// Each direction's angle, in degrees, the screen's way (y down).
const QList<QPair<QString, qreal>> ANGLES = {
    {"right", 0}, {"bottomRight", 45}, {"bottom", 90}, {"bottomLeft", 135},
    {"left", 180}, {"topLeft", -135}, {"top", -90}, {"topRight", -45},
};
constexpr qreal SWIPE = 0.3;      // of the key's smaller side: past this, a touch is a swipe
constexpr qreal MIN_SWIPE = 18;   // px, however small the keys
constexpr qreal NEAREST = 67.5;   // degrees: a swipe this near a key's swipe types it
constexpr qreal CURSOR_STEP = 1.0 / 6;  // of a column: the space bar's slide, per cursor step
// The side column, by page, beside the grid's three rows.
const QStringList SIDE_LETTERS = {"numbers", "shift", "BackSpace"};
const QStringList SIDE_NUMBERS = {"abc", "symbols", "BackSpace"};

qreal angleBetween(qreal a, qreal b)
{
    const qreal d = std::fmod(std::fabs(a - b), 360.0);
    return d > 180 ? 360 - d : d;
}

}  // namespace

bool KeyboardController::isGridPack() const
{
    return m_language.value("layout").toString() == QLatin1String("grid");
}

bool KeyboardController::gridPage() const
{
    return isGridPack() && (m_page == QLatin1String("letters") || m_page == QLatin1String("numbers"));
}

QStringList KeyboardController::pressedCells() const
{
    QStringList out;
    for (const Touch &t : m_touches)
        if (!t.cell.isEmpty())
            out << t.cell;
    return out;
}

void KeyboardController::rebuildGrid()
{
    QVariantList grid;
    QHash<QString, QVariantMap> info;
    if (gridPage()) {
        const bool numbers = m_page == QLatin1String("numbers");
        const QVariantList pack = m_language.value(numbers ? "numbers" : "grid").toList();
        const QStringList &side = numbers ? SIDE_NUMBERS : SIDE_LETTERS;
        auto shown = [this](const QString &t) {
            return t == QLatin1String("shift") ? t : output(t);
        };
        // Text a uinput keyboard cannot type stays off its keys.
        auto typeable = [this](const QString &t) {
            return t == QLatin1String("shift") || !asciiOnly() || m_keyboard->canType(t);
        };
        auto named = [](const QString &k, qreal span) {
            return QVariantMap{{"key", k}, {"center", k}, {"swipes", QVariantMap()}, {"span", span}};
        };
        auto text = [&](const QString &id, const QString &center, const QVariantMap &swipes, qreal span) {
            info.insert(id, {{"center", center}, {"swipes", swipes}});
            QVariantMap shownSwipes;
            for (auto it = swipes.begin(); it != swipes.end(); ++it)
                shownSwipes.insert(it.key(), shown(it.value().toString()));
            return QVariantMap{{"key", id}, {"center", shown(center)}, {"swipes", shownSwipes}, {"span", span}};
        };
        for (int r = 0; r < pack.size(); r++) {
            QVariantList row;
            const QVariantList keys = pack[r].toList();
            for (int c = 0; c < keys.size(); c++) {
                QVariantMap swipes = keys[c].toMap();
                const QString center = swipes.take(QStringLiteral("center")).toString();
                for (auto it = swipes.begin(); it != swipes.end();)
                    it = typeable(it.value().toString()) ? std::next(it) : swipes.erase(it);
                row << text(QStringLiteral("g%1").arg(r * 3 + c), center, swipes, 1);
            }
            row << named(side.value(r), 1);
            grid << QVariant(row);
        }
        // The bottom row, four columns wide: the space bar takes what the
        // others leave. Fn is on the numbers page's, so the letters' space
        // bar stays wide; Enter stays under Backspace, hide at the far left.
        // (Comma: the letters' bottom row; on the numbers page 8 swipes it.)
        QVariantList bottom;
        qreal used = 1 + (numbers ? 0 : 0.5);
        if (m_hideKey) {
            bottom << named(QStringLiteral("hide"), 0.6);
            used += 0.6;
        }
        if (numbers) {
            bottom << named(QStringLiteral("keys"), 0.6) << text(QStringLiteral("t0"), QStringLiteral("0"), {}, 0.6);
            used += 1.2;
        } else if (manyLanguages()) {
            bottom << named(QStringLiteral("lang"), 0.6);
            used += 0.6;
        }
        bottom << named(QStringLiteral("space"), 4 - used);
        if (!numbers)
            bottom << text(QStringLiteral("t,"), QStringLiteral(","), {}, 0.5);
        bottom << named(QStringLiteral("Return"), 1);
        grid << QVariant(bottom);
    }
    m_cellInfo = info;
    if (grid != m_grid) {
        m_grid = grid;
        emit gridChanged();
    }
}

void KeyboardController::registerCell(const QString &key, QQuickItem *item)
{
    m_cells.insert(key, item);
}

void KeyboardController::unregisterCell(const QString &key, QQuickItem *item)
{
    if (m_cells.value(key) == item)
        m_cells.remove(key);
}

QString KeyboardController::cellAt(QPointF pos) const
{
    for (auto it = m_cells.constBegin(); it != m_cells.constEnd(); ++it) {
        QQuickItem *item = it.value();
        // (A page just left may still have its keys, out of the panel.)
        if (item && m_panel && m_panel->isAncestorOf(item) && item->isVisible()
            && item->contains(item->mapFromItem(m_panel, pos)))
            return it.key();
    }
    return {};
}

QString KeyboardController::swipeText(const QString &cell, QPointF move) const
{
    // The key's swipe nearest the way the finger went, if near enough:
    // a key with one swipe takes any swipe roughly its way.
    const QVariantMap swipes = m_cellInfo.value(cell).value("swipes").toMap();
    const qreal angle = qRadiansToDegrees(std::atan2(move.y(), move.x()));
    QString best;
    qreal bestOff = NEAREST;
    for (const auto &[dir, at] : ANGLES) {
        const qreal off = angleBetween(angle, at);
        if (swipes.contains(dir) && off <= bestOff) {
            best = swipes.value(dir).toString();
            bestOff = off;
        }
    }
    return best;
}

void KeyboardController::touchDown(int id, qreal x, qreal y)
{
    Touch t;
    t.start = t.at = QPointF(x, y);
    t.cell = cellAt(t.start);
    if (QQuickItem *item = m_cells.value(t.cell))
        t.size = std::min(item->width(), item->height());
    m_touches.insert(id, t);
    if (t.cell.isEmpty())
        return;
    if (isNamed(t.cell))
        keyDown(t.cell);  // a buzz; Backspace repeats while held
    else
        Haptics::instance()->tick();
    emit pressedChanged();
}

void KeyboardController::touchMove(int id, qreal x, qreal y)
{
    auto it = m_touches.find(id);
    if (it == m_touches.end() || it->cell.isEmpty())
        return;
    Touch &t = *it;
    t.at = QPointF(x, y);
    const QPointF d = t.at - t.start;
    const qreal far = std::max(MIN_SWIPE, SWIPE * t.size);
    if (t.cell == QLatin1String("space")) {
        // Along the space bar (or held: spaceHeld): the cursor, a step at
        // a time.
        if (!t.swiped && std::fabs(d.x()) > far) {
            t.swiped = true;
            startCursor();
        }
        if (!t.swiped)
            return;
        const qreal step = std::max(MIN_SWIPE, CURSOR_STEP * m_keyUnit);
        const int steps = int(d.x() / step);
        while (t.slid < steps) {
            t.slid++;
            cursorStep(QStringLiteral("Right"));
        }
        while (t.slid > steps) {
            t.slid--;
            cursorStep(QStringLiteral("Left"));
        }
        return;
    }
    if (!t.swiped && std::hypot(d.x(), d.y()) > far) {
        t.swiped = true;
        if (isNamed(t.cell))
            keyUp(t.cell);  // no repeat once it is a swipe
        else
            Haptics::instance()->tick();  // felt: a swipe, not a tap
    }
}

void KeyboardController::touchUp(int id, qreal x, qreal y)
{
    touchMove(id, x, y);
    const Touch t = m_touches.take(id);
    if (t.cell.isEmpty())
        return;
    emit pressedChanged();
    const QPointF d = QPointF(x, y) - t.start;
    if (isNamed(t.cell)) {
        const bool repeated = t.cell == m_repeatKey && m_repeated;
        keyUp(t.cell);
        if (t.cell == QLatin1String("space") && t.swiped)
            return;  // it moved the cursor
        if (t.cell == QLatin1String("BackSpace") && t.swiped) {
            if (d.x() < 0 && std::fabs(d.x()) > std::fabs(d.y()))
                press(QStringLiteral("deleteword"));
            return;
        }
        if (!repeated && cellAt(QPointF(x, y)) == t.cell)
            press(t.cell);
        return;
    }
    const QVariantMap info = m_cellInfo.value(t.cell);
    const QString typed = t.swiped ? swipeText(t.cell, d) : info.value("center").toString();
    if (typed.isEmpty())
        return;
    if (typed == QLatin1String("shift")) {
        press(typed);
        return;
    }
    press(output(typed));
}

void KeyboardController::touchCancel(int id)
{
    const Touch t = m_touches.take(id);
    if (t.cell.isEmpty())
        return;
    if (isNamed(t.cell))
        keyUp(t.cell);
    emit pressedChanged();
}
