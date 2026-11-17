#include "Appearance.h"
#include "Workspace.h"

#include <QApplication>
#include <QByteArrayList>
#include <QFontDatabase>
#include <QIcon>
#include <QLockFile>
#include <QMessageBox>
#include <QDir>
#include <QPainter>
#include <QPalette>
#include <QProxyStyle>
#include <QSettings>
#include <QStyleFactory>
#include <QStyleOption>
#include <QStyleHints>

namespace {
const auto themeKey = QStringLiteral("appearance/theme");

class VibeIDEStyle final : public QProxyStyle
{
public:
    VibeIDEStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget = nullptr) const override
    {
        if (element == PE_IndicatorBranch) {
            if (option->state.testFlag(State_Children)) {
                const auto center = QRectF(option->rect).center();
                const bool open = option->state.testFlag(State_Open);
                painter->save();
                painter->setRenderHint(QPainter::Antialiasing);
                painter->setPen(QPen(option->palette.color(QPalette::PlaceholderText), 1.4,
                                     Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter->translate(center);
                painter->rotate(open ? 90 : option->direction == Qt::RightToLeft ? 180 : 0);
                painter->drawPolyline(QPolygonF{QPointF(-1.5, -3), QPointF(1.5, 0), QPointF(-1.5, 3)});
                painter->restore();
            }
            return;
        }
        const bool diffArrow = (element == PE_IndicatorArrowUp || element == PE_IndicatorArrowDown)
            && widget && widget->objectName() == QLatin1String("diffNavigation");
        const bool restoreArrow = element == PE_IndicatorArrowRight
            && widget && widget->objectName() == QLatin1String("restoreDiffHunk");
        if (diffArrow || restoreArrow) {
            const QPointF center = QRectF(option->rect).center();
            const qreal direction = element == PE_IndicatorArrowUp ? -1 : 1;
            const auto role = option->state.testFlag(State_Enabled) ? QPalette::ButtonText : QPalette::PlaceholderText;
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(QPen(option->palette.color(role), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            if (restoreArrow) {
                painter->drawPolyline(QPolygonF{center + QPointF(-4, -3), center + QPointF(-1, 0),
                                                center + QPointF(-4, 3)});
                painter->drawPolyline(QPolygonF{center + QPointF(0, -3), center + QPointF(3, 0),
                                                center + QPointF(0, 3)});
            } else {
                painter->drawPolyline(QPolygonF{center + QPointF(-4, -2 * direction),
                                                center + QPointF(0, 2 * direction),
                                                center + QPointF(4, -2 * direction)});
            }
            painter->restore();
            return;
        }
        if (element != PE_IndicatorTabClose) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }

        const bool enabled = option->state.testFlag(State_Enabled);
        const bool pressed = enabled && option->state.testFlag(State_Sunken);
        const bool hovered = enabled && option->state.testFlag(State_MouseOver);
        const QPointF center = QRectF(option->rect).center();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if (hovered || pressed) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(option->palette.color(pressed ? QPalette::Highlight : QPalette::Midlight));
            painter->drawRoundedRect(QRectF(center.x() - 7, center.y() - 7, 14, 14), 3, 3);
        }

        auto ink = option->palette.color((hovered || pressed) ? QPalette::WindowText : QPalette::PlaceholderText);
        if (!enabled)
            ink.setAlpha(120);
        painter->setPen(QPen(ink, 1.25, Qt::SolidLine, Qt::RoundCap));
        constexpr qreal radius = 3.1;
        painter->drawLine(center + QPointF(-radius, -radius), center + QPointF(radius, radius));
        painter->drawLine(center + QPointF(-radius, radius), center + QPointF(radius, -radius));
        painter->restore();
    }
};

void initializeExecutablePath()
{
    auto directories = qgetenv("PATH").split(':');
    directories.removeAll({});
    for (const QByteArray &directory : {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin"}) {
        if (!directories.contains(directory))
            directories.append(directory);
    }
    qputenv("PATH", directories.join(':'));
}

QPalette themePalette(bool light)
{
    QPalette palette;
    const auto set = [&](QPalette::ColorRole role, const char *dark, const char *bright) {
        palette.setColor(role, QColor(light ? bright : dark));
    };
    set(QPalette::Window, "#202124", "#f7f7f5");
    set(QPalette::WindowText, "#eceef1", "#24262b");
    set(QPalette::Base, "#17181b", "#fefefd");
    set(QPalette::AlternateBase, "#1c1d20", "#f6f6f5");
    set(QPalette::Text, "#eceef1", "#24262b");
    set(QPalette::Button, "#26282d", "#f0f0ef");
    set(QPalette::ButtonText, "#eceef1", "#24262b");
    set(QPalette::Highlight, "#343941", "#e7eaf0");
    set(QPalette::HighlightedText, "#f2f4f8", "#262c35");
    set(QPalette::ToolTipBase, "#2b2d32", "#fafaf8");
    set(QPalette::ToolTipText, "#eceef1", "#24262b");
    set(QPalette::PlaceholderText, "#adb1ba", "#5d626c");
    set(QPalette::Mid, "#34363c", "#dedfdf");
    set(QPalette::Light, "#41434a", "#ffffff");
    set(QPalette::Midlight, "#2d2f35", "#e9e9e8");
    set(QPalette::Dark, "#141519", "#cfd0d2");
    set(QPalette::Shadow, "#0d0e10", "#b1b4bb");
    set(QPalette::Link, "#86cdb3", "#21654f");
    set(QPalette::LinkVisited, "#c3a4ed", "#6c448c");
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, QColor(light ? "#757981" : "#858994"));
    return palette;
}

QString themeStyleSheet(bool light)
{
    auto sheet = QStringLiteral(R"qss(
        QWidget#shell, QWidget#workspace, QWidget#projectStrip, QWidget#footer { background: @canvas; }
        QWidget#sidebar { background: @sidebar; border-right: 1px solid palette(mid); }
        QWidget#projectStrip { border-bottom: 1px solid palette(mid); }
        QWidget#footer { border-top: 1px solid palette(mid); }
        QLabel#brand { color: palette(window-text); font-size: 18px; font-weight: 700; }
        QLabel#sectionLabel { color: palette(placeholder-text); font-size: 10px; font-weight: 600; }
        QLabel#muted, QLabel#sessionStatus, QLabel#diffCommitSummary, QLabel#fileFindCount {
            color: palette(placeholder-text);
        }
        QLabel#sessionStatus, QWidget#footer QLabel { font-size: 11px; }
        QLabel#groupCount { color: palette(placeholder-text); font-size: 11px; }
        QLabel#emptyTitle { color: palette(window-text); font-size: 24px; font-weight: 650; }
        QLabel#editorPath { color: palette(window-text); font-size: 13px; font-weight: 600; }
        QLabel#panelCaption { color: palette(placeholder-text); font-size: 11px; font-weight: 600; }

        QTabWidget#projectViews::pane, QWidget#terminalPanel {
            background: @panel; border: 1px solid palette(mid); border-radius: 10px;
        }
        QTabWidget#terminalTabs::pane { border: 0; border-top: 1px solid palette(mid); }
        QWidget#runBar { background: palette(alternate-base); }
        QSplitter::handle { background: transparent; }
        QSplitter::handle:hover { background: palette(midlight); }

        QPushButton, QToolButton, QComboBox {
            background: palette(button); border: 1px solid palette(mid); border-radius: 6px;
            padding: 5px 10px; color: palette(button-text);
        }
        QPushButton:hover, QToolButton:hover, QComboBox:hover { background: palette(midlight); }
        QPushButton:focus, QToolButton:focus, QComboBox:focus { border-color: palette(link); }
        QPushButton:pressed, QToolButton:pressed, QComboBox:on { background: palette(highlight); }
        QPushButton:disabled, QToolButton:disabled, QComboBox:disabled {
            background: transparent; color: palette(placeholder-text); border-color: palette(mid);
        }
        QComboBox { padding-right: 22px; }
        QComboBox::drop-down { subcontrol-origin: padding; width: 20px; border: 0; }
        QComboBox#diffMode { padding: 0 25px 0 8px; }
        QToolButton#diffNavigation, QToolButton#terminalAdd { padding: 0; }
        QToolButton#restoreDiffHunk, QToolButton#fileFindClose {
            background: transparent; border: 1px solid transparent; padding: 0;
        }
        QToolButton#restoreDiffHunk:hover, QToolButton#fileFindClose:hover { background: palette(midlight); }
        QToolButton#restoreDiffHunk:focus, QToolButton#fileFindClose:focus { border-color: palette(link); }
        QPushButton#gitFetch, QPushButton#gitAdd { background: transparent; border-color: transparent; }
        QPushButton#gitFetch:hover, QPushButton#gitAdd:hover { background: palette(midlight); }
        QPushButton#gitFetch:focus, QPushButton#gitAdd:focus { border-color: palette(link); }
        QPushButton#gitCommit:enabled {
            background: @accentSurface; border-color: @accentBorder; color: palette(link);
            font-weight: 600;
        }
        QPushButton#gitCommit:enabled:hover { background: @accentHover; }
        QPushButton#gitCommit:enabled:focus { border-color: palette(link); }
        QPushButton#runButton:enabled {
            background: #21644b; border-color: #30795c; color: #f1fff7; min-width: 34px;
        }
        QPushButton#runButton:enabled:hover { background: #287659; }
        QPushButton#runButton:enabled:pressed { background: #1a553f; }
        QPushButton#runButton[running="true"]:enabled {
            background: #963f50; border-color: #af5768; color: #fff5f7;
        }
        QPushButton#runButton[running="true"]:enabled:hover { background: #a94d60; }
        QPushButton#runButton[running="true"]:enabled:pressed { background: #823544; }
        QPushButton#runButton:enabled:focus { border-color: palette(link); }
        QPushButton#primary {
            background: @primary; color: @primaryInk; border-color: @primary;
            font-weight: 600; padding: 7px 14px;
        }
        QPushButton#primary:hover { background: @primaryHover; }

        QWidget#groupRow { border: 1px solid transparent; border-radius: 8px; }
        QWidget#groupRow[active="true"] { background: palette(highlight); border-color: palette(mid); }
        QPushButton#groupButton {
            background: transparent; border: 1px solid transparent; text-align: left;
            padding: 4px 6px; font-weight: 600;
        }
        QPushButton#groupButton:hover, QToolButton#groupAdd:hover { background: palette(midlight); }
        QPushButton#groupButton:focus, QToolButton#groupAdd:focus { border-color: palette(link); }
        QPushButton#groupButton:checked { color: palette(highlighted-text); }
        QToolButton#groupAdd { background: transparent; border-color: transparent; padding: 0; }

        QTabBar::tab {
            background: transparent; color: palette(placeholder-text);
            padding: 6px 9px; margin: 2px; border: 1px solid transparent; border-radius: 7px;
        }
        QTabBar::tab:selected { background: palette(midlight); color: palette(window-text); }
        QTabBar::tab:hover:!selected { background: palette(button); color: palette(window-text); }
        QTabBar#contentTabs::tab { padding: 7px 9px; }
        QTabBar#terminalTabBar::tab { padding: 4px 8px; }
        QTabBar QToolButton { padding: 0; }

        QTreeView, QListView, QPlainTextEdit {
            background: palette(base); border: 1px solid palette(mid); border-radius: 6px;
            selection-background-color: palette(highlight); selection-color: palette(highlighted-text);
            outline: 0;
        }
        QTreeView#directoryTree::item, QTreeView#diffFiles::item, QTreeView#currentChanges::item {
            padding: 3px 6px;
        }
        QTreeView::item:hover:!selected, QListView::item:hover:!selected { background: palette(midlight); }
        QTreeView::item:selected, QListView::item:selected { background: palette(highlight); }
        QHeaderView::section {
            background: palette(alternate-base); color: palette(placeholder-text);
            border: 0; border-bottom: 1px solid palette(mid); padding: 6px 8px; font-weight: 600;
        }
        QLineEdit {
            background: palette(base); color: palette(text); border: 1px solid palette(mid);
            border-radius: 6px; padding: 5px 8px;
            selection-background-color: palette(highlight); selection-color: palette(highlighted-text);
        }
        QLineEdit:focus { border-color: palette(link); }
        QWidget#fileFindBar { background: palette(alternate-base); border: 1px solid palette(mid); border-radius: 7px; }
        QLineEdit#fileFindInput { padding: 3px 7px; }

        QScrollBar { background: transparent; border: 0; }
        QScrollBar:vertical { width: 9px; }
        QScrollBar:horizontal { height: 9px; }
        QScrollBar::handle { background: @scrollThumb; border: 2px solid palette(base); border-radius: 4px; }
        QScrollBar::handle:vertical { min-height: 22px; }
        QScrollBar::handle:horizontal { min-width: 22px; }
        QScrollBar::handle:hover { background: @scrollHover; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

        QMenu {
            background: palette(window); border: 1px solid palette(mid);
            border-radius: 7px; padding: 4px;
        }
        QMenu::item { padding: 5px 20px 5px 9px; border-radius: 4px; }
        QMenu::item:selected { background: palette(highlight); color: palette(highlighted-text); }
        QMenu::separator { height: 1px; background: palette(mid); margin: 3px; }
        QToolTip { padding: 4px; border: 1px solid palette(mid); }
    )qss");

    struct Color {
        const char *name;
        const char *dark;
        const char *bright;
    };
    constexpr Color colors[]{
        {"@canvas", "#191a1d", "#f3f3f1"},
        {"@sidebar", "#202124", "#ececea"},
        {"@panel", "#1e1f22", "#fafaf9"},
        {"@accentSurface", "#25342f", "#edf5f1"},
        {"@accentBorder", "#3b574d", "#c4dacf"},
        {"@accentHover", "#2d453a", "#dfede6"},
        {"@primaryInk", "#142b23", "#ffffff"},
        {"@primaryHover", "#a1ddc7", "#28775d"},
        {"@primary", "#86cdb3", "#21654f"},
        {"@scrollThumb", "#5b6069", "#b0b3b8"},
        {"@scrollHover", "#757a85", "#92979f"},
    };
    for (const auto &color : colors)
        sheet.replace(QLatin1String(color.name), QLatin1String(light ? color.bright : color.dark));
    return sheet;
}

void applyTheme(Appearance::Theme theme)
{
    const bool light = theme == Appearance::Theme::Light;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QApplication::styleHints()->setColorScheme(light ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark);
#endif
    QApplication::setPalette(themePalette(light));
    qApp->setStyleSheet(themeStyleSheet(light));
}

void applyAppearance(QApplication &app)
{
    app.setStyle(new VibeIDEStyle);
    auto font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    font.setPointSize(12);
    app.setFont(font);
    applyTheme(Appearance::theme());
}

}


Appearance::Theme Appearance::theme()
{
    QSettings settings;
    auto value = settings.value(themeKey).toString();
    if (!settings.contains(themeKey)) {
        const QSettings previous(QSettings::UserScope, QStringLiteral("VibeAI"), QStringLiteral("VibeAI"));
        value = previous.value(themeKey).toString();
        if (value == QStringLiteral("light") || value == QStringLiteral("dark"))
            settings.setValue(themeKey, value);
    }
    return value == QStringLiteral("light") ? Theme::Light : Theme::Dark;
}

void Appearance::setTheme(Theme theme)
{
    QSettings settings;
    settings.setValue(themeKey, theme == Theme::Light ? QStringLiteral("light") : QStringLiteral("dark"));
    applyTheme(theme);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("VibeIDE"));
    app.setApplicationDisplayName(QStringLiteral("VibeIDE"));
    app.setOrganizationName(QStringLiteral("VibeIDE"));
    app.setApplicationVersion(QStringLiteral("0.1.6"));
    const QDir executableDirectory(app.applicationDirPath());
    app.setWindowIcon(QIcon(executableDirectory.absoluteFilePath(QStringLiteral("../Resources/VibeIDE.png"))));
    initializeExecutablePath();
    applyAppearance(app);

    const auto directory = Workspace::stateDirectory();
    if (directory.isEmpty() || !QDir().mkpath(directory)) {
        QMessageBox::critical(nullptr, QObject::tr("Workspace Unavailable"),
                              QObject::tr("Could not create the workspace settings directory."));
        return 1;
    }
    QLockFile lock(directory + "/workspace.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) {
        QMessageBox::warning(nullptr, QObject::tr("Workspace Unavailable"),
                             QObject::tr("Close the other VibeIDE window or check the workspace folder permissions."));
        return 1;
    }

    Workspace workspace;
    workspace.show();
    return app.exec();
}
