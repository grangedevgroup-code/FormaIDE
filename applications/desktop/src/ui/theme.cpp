#include "ui/theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPainter>
#include <QDir>
#include <QPalette>
#include <QtMath>
#include <QStyleFactory>
#include <QStyleHints>
#include <QSvgRenderer>

#include "forma/core/paths.h"
#include "forma/resources/resources.h"

namespace forma::ui {

namespace {

ThemeColors lightColors()
{
    ThemeColors c;
    c.window = QColor("#ECEEF1");
    c.panel = QColor("#F6F7F9");
    c.panelRaised = QColor("#FFFFFF");
    c.editor = QColor("#FFFFFF");
    c.canvas = QColor("#E4E6EA");
    c.border = QColor("#D8DBE1");
    c.borderStrong = QColor("#B9BEC8");
    c.text = QColor("#1C1F26");
    c.textMuted = QColor("#5A6170");
    c.textFaint = QColor("#8A90A0");
    c.accent = QColor("#2B6CD4");
    c.accentHover = QColor("#2360C0");
    c.accentText = QColor("#FFFFFF");
    c.hover = QColor(0, 0, 0, 14);
    c.pressed = QColor(0, 0, 0, 26);
    c.selection = QColor("#D5E3F8");
    c.selectionText = QColor("#12243F");
    c.danger = QColor("#C42F2A");
    c.warning = QColor("#A86A00");
    c.success = QColor("#2C7A3F");
    c.generated = QColor("#7A5AA6");
    return c;
}

ThemeColors darkColors()
{
    ThemeColors c;
    c.window = QColor("#17191E");
    c.panel = QColor("#1F2127");
    c.panelRaised = QColor("#272A31");
    c.editor = QColor("#1B1D22");
    c.canvas = QColor("#121317");
    c.border = QColor("#2F323A");
    c.borderStrong = QColor("#454954");
    c.text = QColor("#E2E4E9");
    c.textMuted = QColor("#9CA2AF");
    c.textFaint = QColor("#6E7482");
    c.accent = QColor("#4C8DFF");
    c.accentHover = QColor("#6AA0FF");
    c.accentText = QColor("#0C1422");
    c.hover = QColor(255, 255, 255, 16);
    c.pressed = QColor(255, 255, 255, 28);
    c.selection = QColor("#27416B");
    c.selectionText = QColor("#E6EEFB");
    c.danger = QColor("#F0716B");
    c.warning = QColor("#E0A93A");
    c.success = QColor("#5CBF78");
    c.generated = QColor("#B79AE0");
    return c;
}

QString css(const QColor& color)
{
    if (color.alpha() == 255)
        return color.name();
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alphaF(), 0, 'f', 3);
}

}

Theme& Theme::instance()
{
    static Theme theme;
    return theme;
}

void Theme::apply(QApplication& application)
{
    application_ = &application;
    application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    connect(application.styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (mode_ == QStringLiteral("system"))
            refresh();
    });
    refresh();
}

void Theme::setMode(const QString& mode)
{
    mode_ = mode;
    refresh();
}

void Theme::refresh()
{
    bool dark = mode_ == QStringLiteral("dark");
    if (mode_ == QStringLiteral("system") && application_ != nullptr)
        dark = application_->styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    dark_ = dark;
    colors_ = dark ? darkColors() : lightColors();
    if (application_ != nullptr) {
        QPalette palette;
        palette.setColor(QPalette::Window, colors_.panel);
        palette.setColor(QPalette::WindowText, colors_.text);
        palette.setColor(QPalette::Base, colors_.panelRaised);
        palette.setColor(QPalette::AlternateBase, colors_.panel);
        palette.setColor(QPalette::Text, colors_.text);
        palette.setColor(QPalette::PlaceholderText, colors_.textFaint);
        palette.setColor(QPalette::Button, colors_.panelRaised);
        palette.setColor(QPalette::ButtonText, colors_.text);
        palette.setColor(QPalette::BrightText, colors_.accentText);
        palette.setColor(QPalette::Highlight, colors_.selection);
        palette.setColor(QPalette::HighlightedText, colors_.selectionText);
        palette.setColor(QPalette::ToolTipBase, colors_.panelRaised);
        palette.setColor(QPalette::ToolTipText, colors_.text);
        palette.setColor(QPalette::Link, colors_.accent);
        palette.setColor(QPalette::LinkVisited, colors_.accent);
        palette.setColor(QPalette::Disabled, QPalette::Text, colors_.textFaint);
        palette.setColor(QPalette::Disabled, QPalette::WindowText, colors_.textFaint);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, colors_.textFaint);
        application_->setPalette(palette);
        application_->setStyleSheet(styleSheet());
    }
    Q_EMIT changed();
}

QFont Theme::codeFont(int pointSize) const
{
    static const QStringList candidates = {QStringLiteral("Cascadia Mono"), QStringLiteral("SF Mono"), QStringLiteral("JetBrains Mono"),
                                           QStringLiteral("Menlo"), QStringLiteral("Consolas"), QStringLiteral("DejaVu Sans Mono"),
                                           QStringLiteral("Noto Sans Mono"), QStringLiteral("Liberation Mono")};
    QStringList families = QFontDatabase::families();
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    for (const auto& name : candidates) {
        if (families.contains(name)) {
            font = QFont(name);
            break;
        }
    }
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    if (pointSize > 0)
        font.setPointSize(pointSize);
    return font;
}

QPixmap Theme::pixmap(const QString& name, int size, const QColor& tint, qreal ratio) const
{
    auto data = resources::find(("icons/" + name + ".svg").toStdString());
    if (!data)
        return {};
    QColor color = tint.isValid() ? tint : colors_.textMuted;
    QByteArray svg(data->data(), static_cast<qsizetype>(data->size()));
    svg.replace("currentColor", color.name().toLatin1());
    if (color.alpha() < 255)
        svg.replace("<svg ", QStringLiteral("<svg opacity=\"%1\" ").arg(color.alphaF(), 0, 'f', 2).toLatin1());
    QSvgRenderer renderer(svg);
    int pixels = qCeil(size * ratio);
    QPixmap result(pixels, pixels);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    renderer.render(&painter, QRectF(0, 0, pixels, pixels));
    painter.end();
    result.setDevicePixelRatio(ratio);
    return result;
}

QIcon Theme::icon(const QString& name, const QColor& tint) const
{
    QIcon output;
    for (int size : {16, 20, 24, 32}) {
        for (qreal ratio : {1.0, 2.0}) {
            output.addPixmap(pixmap(name, size, tint, ratio), QIcon::Normal);
            output.addPixmap(pixmap(name, size, colors_.textFaint, ratio), QIcon::Disabled);
            if (!tint.isValid()) {
                output.addPixmap(pixmap(name, size, colors_.accent, ratio), QIcon::Normal, QIcon::On);
                output.addPixmap(pixmap(name, size, colors_.text, ratio), QIcon::Active);
            }
        }
    }
    return output;
}

QString Theme::styleSheet() const
{
    const ThemeColors& c = colors_;
    QString sheet = QStringLiteral(R"QSS(
* { outline: 0; }
QMainWindow, QDialog { background: %1; }
QMainWindow::separator { background: %2; width: 1px; height: 1px; }
QMainWindow::separator:hover { background: %3; }
QWidget { color: %4; }
QToolTip { background: %5; color: %4; border: 1px solid %6; padding: 3px 6px; }
QLabel[role="muted"] { color: %7; }
QLabel[role="faint"] { color: %8; }
QLabel[role="heading"] { font-size: 16pt; font-weight: 600; }
QLabel[role="subheading"] { font-weight: 600; }
QLabel[role="section"] { color: %7; font-weight: 600; }
QLabel[role="error"] { color: %9; }
QToolBar { background: %10; border: none; border-bottom: 1px solid %2; spacing: 2px; padding: 3px 6px; }
QToolBar::separator { background: %2; width: 1px; margin: 4px 6px; }
QToolBar[role="plain"] { background: transparent; border: none; padding: 2px 4px; }
QToolButton { background: transparent; border: none; border-radius: 4px; padding: 4px; color: %4; }
QToolButton:hover { background: %11; }
QToolButton:pressed { background: %12; }
QToolButton:checked { background: %13; color: %14; }
QToolButton:disabled { color: %8; }
QToolButton::menu-indicator { image: none; width: 0; }
QToolButton[role="text"] { padding: 4px 8px; }
QPushButton { background: %5; border: 1px solid %6; border-radius: 4px; padding: 4px 12px; min-height: 18px; }
QPushButton:hover { background: %15; border-color: %3; }
QPushButton:pressed { background: %12; }
QPushButton:disabled { color: %8; border-color: %2; }
QPushButton[role="primary"] { background: %16; border: 1px solid %16; color: %17; font-weight: 600; }
QPushButton[role="primary"]:hover { background: %18; border-color: %18; }
QPushButton[role="primary"]:disabled { background: %2; border-color: %2; color: %8; }
QPushButton[role="danger"] { color: %9; }
QPushButton[role="link"] { background: transparent; border: none; color: %16; padding: 2px 0; text-align: left; }
QPushButton[role="link"]:hover { text-decoration: underline; background: transparent; }
QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {
    background: %5; border: 1px solid %6; border-radius: 4px; padding: 3px 6px; selection-background-color: %13; selection-color: %14;
}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: %16; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled { color: %8; background: %10; }
QLineEdit[role="search"] { padding-left: 6px; }
QTabBar::close-button { image: url(@CLOSE@); subcontrol-position: right; width: 12px; height: 12px; margin: 2px; border-radius: 3px; }
QTabBar::close-button:hover { background: %11; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox::down-arrow { image: url(@CHEVRON@); width: 10px; height: 10px; margin-right: 6px; }
QComboBox QAbstractItemView { background: %5; border: 1px solid %6; selection-background-color: %13; selection-color: %14; outline: 0; }
QSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 0; border: none; }
QCheckBox { spacing: 6px; }
QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid %3; border-radius: 3px; background: %5; }
QCheckBox::indicator:checked { background: %16; border-color: %16; image: url(@CHECK@); }
QCheckBox::indicator:hover { border-color: %16; }
QRadioButton::indicator { width: 14px; height: 14px; border: 1px solid %3; border-radius: 7px; background: %5; }
QRadioButton::indicator:checked { border: 4px solid %16; background: %5; }
QTreeView, QListView, QTableView { background: %10; border: none; alternate-background-color: %10; selection-background-color: %13; selection-color: %14; show-decoration-selected: 1; }
QTreeView::item, QListView::item { padding: 2px 2px; border: none; }
QTreeView::item:hover, QListView::item:hover { background: %11; }
QTreeView::item:selected, QListView::item:selected, QTreeView::branch:selected { background: %13; color: %14; }
QTreeView::branch { background: transparent; }
QHeaderView { background: %10; }
QHeaderView::section { background: %10; color: %7; border: none; border-bottom: 1px solid %2; border-right: 1px solid %2; padding: 3px 6px; font-weight: 600; }
QTabWidget::pane { border: none; }
QTabBar { background: %10; qproperty-drawBase: 0; }
QTabBar::tab { background: transparent; color: %7; padding: 5px 12px; border: none; border-bottom: 2px solid transparent; }
QTabBar::tab:hover { color: %4; background: %11; }
QTabBar::tab:selected { color: %4; border-bottom: 2px solid %16; }
QTabBar::close-button { subcontrol-position: right; margin-left: 4px; }
QDockWidget { titlebar-close-icon: none; titlebar-normal-icon: none; }
QDockWidget::title { background: %10; padding: 4px 8px; border-bottom: 1px solid %2; }
QSplitter::handle { background: %2; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle { background: %3; border-radius: 3px; margin: 2px; min-height: 24px; min-width: 24px; }
QScrollBar::handle:hover { background: %7; }
QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { background: none; border: none; width: 0; height: 0; }
QMenuBar { background: %10; border-bottom: 1px solid %2; padding: 0 4px; }
QMenuBar::item { background: transparent; padding: 4px 8px; border-radius: 3px; }
QMenuBar::item:selected { background: %11; }
QMenu { background: %5; border: 1px solid %6; padding: 4px; }
QMenu::item { padding: 4px 24px 4px 8px; border-radius: 3px; }
QMenu::item:selected { background: %13; color: %14; }
QMenu::item:disabled { color: %8; }
QMenu::separator { height: 1px; background: %2; margin: 4px 2px; }
QMenu::icon { padding-left: 4px; }
QStatusBar { background: %10; border-top: 1px solid %2; color: %7; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: %7; padding: 0 6px; }
QStatusBar QToolButton { padding: 1px 6px; color: %7; }
QProgressBar { background: %2; border: none; border-radius: 2px; height: 4px; text-align: center; color: transparent; }
QProgressBar::chunk { background: %16; border-radius: 2px; }
QGroupBox { border: 1px solid %2; border-radius: 4px; margin-top: 10px; padding-top: 6px; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: %7; }
QFrame[role="panelHeader"] { background: %10; border-bottom: 1px solid %2; }
QFrame[role="line"] { background: %2; max-height: 1px; min-height: 1px; border: none; }
QFrame[role="card"] { background: %5; border: 1px solid %2; border-radius: 6px; }
QFrame[role="banner"] { background: %13; border-bottom: 1px solid %2; }
QFrame[role="bannerWarning"] { background: %19; border-bottom: 1px solid %2; }
QWidget#workspaceRail { background: %1; border-right: 1px solid %2; }
QToolButton[role="rail"] { border-radius: 4px; padding: 6px 2px 4px 2px; color: %7; font-size: 8pt; }
QToolButton[role="rail"]:hover { background: %11; color: %4; }
QToolButton[role="rail"]:checked { background: %13; color: %14; font-weight: 600; }
)QSS");
    QColor warningFill = c.warning;
    warningFill.setAlpha(dark_ ? 46 : 36);
    sheet.replace(QStringLiteral("%19"), css(warningFill));
    sheet.replace(QStringLiteral("%18"), css(c.accentHover));
    sheet.replace(QStringLiteral("%17"), css(c.accentText));
    sheet.replace(QStringLiteral("%16"), css(c.accent));
    sheet.replace(QStringLiteral("%15"), css(dark_ ? QColor("#30333B") : QColor("#F0F1F4")));
    sheet.replace(QStringLiteral("%14"), css(c.selectionText));
    sheet.replace(QStringLiteral("%13"), css(c.selection));
    sheet.replace(QStringLiteral("%12"), css(c.pressed));
    sheet.replace(QStringLiteral("%11"), css(c.hover));
    sheet.replace(QStringLiteral("%10"), css(c.panel));
    sheet.replace(QStringLiteral("%9"), css(c.danger));
    sheet.replace(QStringLiteral("%8"), css(c.textFaint));
    sheet.replace(QStringLiteral("%7"), css(c.textMuted));
    sheet.replace(QStringLiteral("%6"), css(c.borderStrong));
    sheet.replace(QStringLiteral("%5"), css(c.panelRaised));
    sheet.replace(QStringLiteral("%4"), css(c.text));
    sheet.replace(QStringLiteral("%3"), css(c.borderStrong));
    sheet.replace(QStringLiteral("%2"), css(c.border));
    sheet.replace(QStringLiteral("%1"), css(c.window));
    auto cached = [this](const QString& name, const QColor& tint) {
        QDir directory(QString::fromStdString(paths::toUtf8(paths::userCacheDir() / "theme")));
        directory.mkpath(QStringLiteral("."));
        QString file = directory.filePath(QStringLiteral("%1-%2.png").arg(name, tint.name().mid(1)));
        pixmap(name, 28, tint).save(file);
        return file;
    };
    sheet.replace(QStringLiteral("@CHEVRON@"), cached(QStringLiteral("chevron-down"), c.textMuted));
    sheet.replace(QStringLiteral("@CLOSE@"), cached(QStringLiteral("close"), c.textMuted));
    sheet.replace(QStringLiteral("@CHECK@"), cached(QStringLiteral("check"), c.accentText));
    return sheet;
}

}
