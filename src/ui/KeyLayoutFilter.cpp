#include "ui/KeyLayoutFilter.h"

#include <QAction>
#include <QApplication>
#include <QChar>
#include <QHash>
#include <QKeyEvent>
#include <QKeySequence>
#include <QWidget>

namespace tessel {

namespace {

// The Russian layout, by physical key: the letter it produces against the
// letter printed on the same key. Only the three letter rows and the few
// punctuation keys that carry a Cyrillic letter need listing.
const QHash<int, int>& layoutMap()
{
    static const QHash<int, int> map = {
        {0x0419, Qt::Key_Q},  {0x0426, Qt::Key_W},  {0x0423, Qt::Key_E},
        {0x041A, Qt::Key_R},  {0x0415, Qt::Key_T},  {0x041D, Qt::Key_Y},
        {0x0413, Qt::Key_U},  {0x0428, Qt::Key_I},  {0x0429, Qt::Key_O},
        {0x0417, Qt::Key_P},  {0x0425, Qt::Key_BracketLeft},
        {0x042A, Qt::Key_BracketRight},
        {0x0424, Qt::Key_A},  {0x042B, Qt::Key_S},  {0x0412, Qt::Key_D},
        {0x0410, Qt::Key_F},  {0x041F, Qt::Key_G},  {0x0420, Qt::Key_H},
        {0x041E, Qt::Key_J},  {0x041B, Qt::Key_K},  {0x0414, Qt::Key_L},
        {0x0416, Qt::Key_Semicolon}, {0x042D, Qt::Key_Apostrophe},
        {0x042F, Qt::Key_Z},  {0x0427, Qt::Key_X},  {0x0421, Qt::Key_C},
        {0x041C, Qt::Key_V},  {0x0418, Qt::Key_B},  {0x0422, Qt::Key_N},
        {0x042C, Qt::Key_M},  {0x0411, Qt::Key_Comma}, {0x042E, Qt::Key_Period},
        {0x0401, Qt::Key_QuoteLeft},
    };
    return map;
}

// Walks the window for an action bound to this sequence and fires it.
bool triggerShortcut(QWidget* window, const QKeySequence& wanted)
{
    const QList<QAction*> actions =
        window->findChildren<QAction*>(QString(), Qt::FindChildrenRecursively);
    for (QAction* action : actions) {
        for (const QKeySequence& bound : action->shortcuts()) {
            if (bound != wanted)
                continue;
            if (!action->isEnabled())
                return true;  // bound, but not available right now
            action->trigger();
            return true;
        }
    }
    return false;
}

} // namespace

int KeyLayoutFilter::latinKeyFor(int key)
{
    if (key <= 0x7f)
        return 0;
    const int upper = QChar(char32_t(key)).toUpper().unicode();
    return layoutMap().value(upper, 0);
}

bool KeyLayoutFilter::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::KeyPress)
        return QObject::eventFilter(watched, event);
    auto* key = static_cast<QKeyEvent*>(event);
    // Only sequences that hold down Ctrl or Alt. A plain letter is text the
    // user is typing, and swallowing it would break the text tool.
    const Qt::KeyboardModifiers mods = key->modifiers();
    if (!mods.testAnyFlags(Qt::ControlModifier | Qt::AltModifier))
        return QObject::eventFilter(watched, event);
    const int latin = latinKeyFor(key->key());
    if (latin == 0)
        return QObject::eventFilter(watched, event);
    QWidget* window = QApplication::activeWindow();
    if (!window)
        return QObject::eventFilter(watched, event);
    const QKeySequence wanted(QKeyCombination(mods, Qt::Key(latin)));
    if (triggerShortcut(window, wanted)) {
        event->accept();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace tessel
