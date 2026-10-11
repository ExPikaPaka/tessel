#include "ui/Theme.h"

#include <QApplication>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>

namespace tessel::theme {

namespace {

Mode activeMode = Mode::Dark;

// Clicking the groove of a slider normally nudges it by one page and stops
// there, so reaching a value takes several clicks and the handle cannot be
// dragged from where it landed. This makes the left button jump the handle
// straight to the click and keep following the pointer, which is what a value
// slider is for. The middle button keeps the paging behaviour.
class SliderStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
                  QStyleHintReturn* returnData) const override
    {
        if (hint == SH_Slider_AbsoluteSetButtons)
            return Qt::LeftButton;
        if (hint == SH_Slider_PageSetButtons)
            return Qt::MiddleButton;
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
};

struct Colors {
    QColor window;
    QColor panel;
    QColor base;
    QColor text;
    QColor dimText;
    QColor accent;
    QColor border;
    QColor backdrop;
    QColor checkerLight;
    QColor checkerDark;
    QColor grid;
};

const Colors& colors()
{
    static const Colors dark = {
        QColor(0x2b, 0x2d, 0x31), QColor(0x33, 0x36, 0x3b), QColor(0x22, 0x24, 0x27),
        QColor(0xe6, 0xe8, 0xeb), QColor(0x9a, 0xa0, 0xa8), QColor(0x3d, 0x8b, 0xfd),
        QColor(0x1d, 0x1f, 0x22), QColor(0x1a, 0x1b, 0x1e),
        QColor(0xc8, 0xc8, 0xc8), QColor(0x96, 0x96, 0x96), QColor(0x60, 0x63, 0x68),
    };
    static const Colors light = {
        QColor(0xef, 0xf1, 0xf3), QColor(0xe4, 0xe7, 0xea), QColor(0xff, 0xff, 0xff),
        QColor(0x1c, 0x1e, 0x21), QColor(0x5c, 0x62, 0x6a), QColor(0x1a, 0x6d, 0xe0),
        QColor(0xc6, 0xca, 0xd0), QColor(0x80, 0x84, 0x8a),
        QColor(0xff, 0xff, 0xff), QColor(0xd0, 0xd0, 0xd0), QColor(0xa8, 0xac, 0xb2),
    };
    return activeMode == Mode::Dark ? dark : light;
}

QString buildStyleSheet()
{
    const Colors& c = colors();
    const QString window = c.window.name();
    const QString panel = c.panel.name();
    const QString base = c.base.name();
    const QString text = c.text.name();
    const QString dim = c.dimText.name();
    const QString accent = c.accent.name();
    const QString border = c.border.name();

    return QStringLiteral(R"(
QWidget { font-size: 10pt; }
QMainWindow, QDialog { background: %1; color: %3; }
QMenuBar { background: %1; color: %3; border-bottom: 1px solid %6; padding: 2px; }
QMenuBar::item { padding: 4px 9px; border-radius: 4px; background: transparent; }
QMenuBar::item:selected { background: %5; color: white; }
QMenu { background: %2; color: %3; border: 1px solid %6; padding: 4px; }
QMenu::item { padding: 5px 26px 5px 24px; border-radius: 4px; }
QMenu::item:selected { background: %5; color: white; }
QMenu::item:disabled { color: %4; }
QMenu::separator { height: 1px; background: %6; margin: 4px 6px; }
QMenu::icon { padding-left: 6px; }
QToolBar { background: %1; border: 0; border-bottom: 1px solid %6; spacing: 2px; padding: 3px; }
QToolBar::separator { width: 1px; background: %6; margin: 3px 5px; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 5px;
              padding: 3px; color: %3; }
QToolButton:hover { background: %2; border-color: %6; }
QToolButton:pressed, QToolButton:checked { background: %5; border-color: %5; color: white; }
QToolButton:disabled { color: %4; }
QStatusBar { background: %1; color: %4; border-top: 1px solid %6; }
QStatusBar QLabel { color: %4; padding: 0 6px; }
QDockWidget { color: %3; }
QDockWidget::close-button, QDockWidget::float-button {
    background: transparent; border: 1px solid transparent; border-radius: 3px;
    padding: 0; margin: 2px; }
QDockWidget::close-button:hover, QDockWidget::float-button:hover {
    background: %5; border-color: %5; }
/* The title bar buttons are real widgets, direct children of the dock, so the
   pseudo element rules above do not reach them once they carry our own icons.
   A panel's own buttons sit inside its content widget and are left alone. */
QDockWidget > QAbstractButton {
    background: transparent; border: 1px solid transparent; border-radius: 3px; }
QDockWidget > QAbstractButton:hover { background: %5; border-color: %5; }
QDockWidget > QAbstractButton:pressed { background: %6; border-color: %6; }
QDockWidget::title { background: %2; padding: 5px 8px; border-bottom: 1px solid %6;
                     text-align: left; font-weight: 600; }
QDockWidget > QWidget { background: %1; }
QTabBar { background: %1; }
QTabBar::tab { background: %2; color: %4; padding: 5px 10px; margin-right: 2px;
               border: 1px solid %6; border-bottom: 0;
               border-top-left-radius: 5px; border-top-right-radius: 5px; }
QTabBar::tab:selected { background: %1; color: %3; }
QTabBar::tab:hover { color: %3; }
QTabBar::close-button { image: none; }
QListWidget, QTreeWidget, QTableWidget { background: %2; color: %3; border: 1px solid %6;
                                         border-radius: 5px; outline: 0; }
QListWidget::item { padding: 3px; border-radius: 4px; }
QListWidget::item:selected { background: %5; color: white; }
QListWidget::item:hover:!selected { background: %1; }
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit {
    background: %2; color: %3; border: 1px solid %6; border-radius: 5px; padding: 3px 6px;
    selection-background-color: %5; min-height: 18px; }
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: %5; }
QComboBox::drop-down { border: 0; width: 16px; }
QComboBox QAbstractItemView { background: %2; color: %3; border: 1px solid %6;
                              selection-background-color: %5; }
QPushButton { background: %2; color: %3; border: 1px solid %6; border-radius: 5px;
              padding: 5px 14px; min-width: 64px; }
QPushButton:hover { border-color: %5; }
QPushButton:pressed { background: %5; color: white; }
QPushButton:default { border-color: %5; }
QPushButton:disabled { color: %4; }
QCheckBox, QRadioButton, QLabel, QGroupBox { color: %3; }
QGroupBox { border: 1px solid %6; border-radius: 5px; margin-top: 10px; padding-top: 8px; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: %4; }
QSlider::groove:horizontal { height: 18px; background: %2; border: 1px solid %6;
                             border-radius: 3px; }
QSlider::sub-page:horizontal { background: %5; border: 1px solid %5;
                               border-top-left-radius: 2px;
                               border-bottom-left-radius: 2px; }
QSlider::add-page:horizontal { background: %2; border-top-right-radius: 2px;
                               border-bottom-right-radius: 2px; }
QSlider::handle:horizontal { background: %3; border: 1px solid %6; width: 7px;
                             margin: -1px 0; border-radius: 2px; }
QSlider::handle:horizontal:hover { background: %5; border-color: %3; }
QSlider::groove:vertical { width: 18px; background: %2; border: 1px solid %6;
                           border-radius: 3px; }
QSlider::sub-page:vertical { background: %2; }
QSlider::add-page:vertical { background: %5; }
QSlider::handle:vertical { background: %3; border: 1px solid %6; height: 7px;
                           margin: 0 -1px; border-radius: 2px; }
QScrollBar:vertical { background: %1; width: 11px; margin: 0; }
QScrollBar:horizontal { background: %1; height: 11px; margin: 0; }
QScrollBar::handle { background: %6; border-radius: 5px; min-height: 24px; min-width: 24px; }
QScrollBar::handle:hover { background: %4; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QToolTip { background: %2; color: %3; border: 1px solid %6; padding: 3px; }
QSplitter::handle { background: %6; }
QProgressBar { border: 1px solid %6; border-radius: 4px; background: %2; text-align: center;
               color: %3; }
QProgressBar::chunk { background: %5; border-radius: 3px; }
)")
        .arg(window, panel, text, dim, accent, border);
}

} // namespace

void apply(QApplication& app, Mode mode)
{
    activeMode = mode;
    const Colors& c = colors();
    app.setStyle(new SliderStyle(QStyleFactory::create(QStringLiteral("Fusion"))));

    QPalette palette;
    palette.setColor(QPalette::Window, c.window);
    palette.setColor(QPalette::WindowText, c.text);
    palette.setColor(QPalette::Base, c.base);
    palette.setColor(QPalette::AlternateBase, c.panel);
    palette.setColor(QPalette::ToolTipBase, c.panel);
    palette.setColor(QPalette::ToolTipText, c.text);
    palette.setColor(QPalette::Text, c.text);
    palette.setColor(QPalette::Button, c.panel);
    palette.setColor(QPalette::ButtonText, c.text);
    palette.setColor(QPalette::BrightText, Qt::red);
    palette.setColor(QPalette::Link, c.accent);
    palette.setColor(QPalette::Highlight, c.accent);
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::PlaceholderText, c.dimText);
    palette.setColor(QPalette::Disabled, QPalette::Text, c.dimText);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, c.dimText);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, c.dimText);
    app.setPalette(palette);
    app.setStyleSheet(buildStyleSheet());
}

Mode current() { return activeMode; }

QColor windowColor() { return colors().window; }
QColor panelColor() { return colors().panel; }
QColor accentColor() { return colors().accent; }
QColor textColor() { return colors().text; }
QColor canvasBackdropColor() { return colors().backdrop; }
QColor checkerLight() { return colors().checkerLight; }
QColor checkerDark() { return colors().checkerDark; }
QColor gridColor() { return colors().grid; }

} // namespace tessel::theme
