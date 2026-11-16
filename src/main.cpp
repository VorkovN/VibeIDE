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
        const bool diffArrow = (element == PE_IndicatorArrowUp || element == PE_IndicatorArrowDown)
            && widget && widget->objectName() == QLatin1String("diffNavigation");
        if (diffArrow) {
            const QPointF center = QRectF(option->rect).center();
            const qreal direction = element == PE_IndicatorArrowUp ? -1 : 1;
            const auto role = option->state.testFlag(State_Enabled) ? QPalette::ButtonText : QPalette::PlaceholderText;
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(QPen(option->palette.color(role), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->drawPolyline(QPolygonF{center + QPointF(-4, -2 * direction),
                                            center + QPointF(0, 2 * direction),
                                            center + QPointF(4, -2 * direction)});
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
        const bool dark = option->palette.color(QPalette::Base).lightness() < 128;
        const QPointF center = QRectF(option->rect).center();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if (hovered || pressed) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(dark ? (pressed ? "#29364b" : "#3a4962") : (pressed ? "#c4d3e7" : "#dde7f3")));
            painter->drawRoundedRect(QRectF(center.x() - 7, center.y() - 7, 14, 14), 3, 3);
        }

        QColor ink(dark ? "#a9b6cc" : "#52647d");
        if (hovered || pressed)
            ink = QColor(dark ? "#e7ebf2" : "#253b59");
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
    set(QPalette::Window, "#1b212d", "#f7f9fc");
    set(QPalette::WindowText, "#e3e8f2", "#202941");
    set(QPalette::Base, "#151b25", "#ffffff");
    set(QPalette::AlternateBase, "#19212e", "#f2f6fb");
    set(QPalette::Text, "#e3e8f2", "#202941");
    set(QPalette::Button, "#253044", "#f9fbfd");
    set(QPalette::ButtonText, "#e3e8f2", "#202941");
    set(QPalette::Highlight, "#374e7c", "#dbe7fd");
    set(QPalette::HighlightedText, "#ffffff", "#192d52");
    set(QPalette::ToolTipBase, "#253044", "#f9fbfd");
    set(QPalette::ToolTipText, "#e3e8f2", "#202941");
    set(QPalette::PlaceholderText, "#a5b4ce", "#52647d");
    set(QPalette::Mid, "#2d3a50", "#cbd7e7");
    set(QPalette::Light, "#39485d", "#ffffff");
    set(QPalette::Midlight, "#293548", "#f0f4f9");
    set(QPalette::Dark, "#151b25", "#b9c7db");
    set(QPalette::Shadow, "#0d1119", "#96a6bd");
    set(QPalette::Link, "#8fc5ff", "#284f90");
    set(QPalette::LinkVisited, "#c398f4", "#6940a0");
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, QColor(light ? "#6a778b" : "#8995aa"));
    return palette;
}

QString themeStyleSheet(bool light)
{
    auto sheet = QStringLiteral(R"qss(
        QWidget#workspace { background: #111721; }
        QWidget#sidebar { background: #151b25; border-right: 1px solid palette(mid); }
        QWidget#projectStrip, QWidget#footer { background: #1a2230; }
        QWidget#projectStrip { border-bottom: 1px solid palette(mid); }
        QWidget#footer { border-top: 1px solid palette(mid); }
        QLabel#brand { color: palette(window-text); font-size: 14px; font-weight: 700; }
        QLabel#sectionLabel { color: palette(placeholder-text); font-size: 10px; font-weight: 700; }
        QLabel#muted, QLabel#sessionStatus, QLabel#groupCount { color: palette(placeholder-text); }
        QLabel#groupCount { min-width: 19px; font-size: 11px; }
        QLabel#emptyTitle { color: palette(window-text); font-size: 23px; font-weight: 600; }
        QTabWidget#projectViews, QWidget#terminalPanel {
            background: #1b2331; border: 1px solid palette(mid); border-radius: 6px;
        }
        QWidget#runBar { background: #222c3d; }
        QPushButton, QToolButton, QComboBox#diffMode {
            background: #273348; border: 1px solid palette(mid);
            border-radius: 4px; padding: 5px 9px;
        }
        QPushButton:hover, QToolButton:hover, QComboBox#diffMode:hover { background: #354563; }
        QPushButton:focus, QToolButton:focus, QComboBox#diffMode:focus { border-color: #86a7eb; }
        QPushButton:pressed, QToolButton:pressed, QComboBox#diffMode:on { background: #1b2538; }
        QPushButton:disabled, QToolButton:disabled, QComboBox#diffMode:disabled {
            background: #222a38; color: palette(placeholder-text);
        }
        QToolButton#diffNavigation { padding: 0; }
        QComboBox#diffMode { padding: 0 25px 0 9px; }
        QComboBox#diffMode::drop-down { subcontrol-origin: padding; width: 24px; border: 0; }
        QPushButton#runButton:enabled {
            background: #24704e; border-color: #3a956c; color: #f2fff8; min-width: 34px;
        }
        QPushButton#runButton:enabled:hover { background: #2d805b; }
        QPushButton#runButton:enabled:pressed { background: #1e5d41; }
        QPushButton#runButton[running="true"]:enabled {
            background: #a64452; border-color: #d06571; color: #fff5f6;
        }
        QPushButton#runButton[running="true"]:enabled:hover { background: #b54d5c; }
        QPushButton#runButton[running="true"]:enabled:pressed { background: #873844; }
        QPushButton#runButton:enabled:focus { border-color: #b4c9ee; }
        QPushButton#primary {
            background: #a9c6ff; color: #16243f; border-color: #b9d1ff;
            font-weight: 700; padding: 8px 16px;
        }
        QPushButton#primary:hover { background: #c5d8ff; }
        QWidget#groupRow { border: 1px solid transparent; border-radius: 4px; }
        QWidget#groupRow[active="true"] { background: #26364f; border-color: #3c5273; }
        QToolButton#groupButton {
            background: transparent; border: 1px solid transparent;
            text-align: left; padding: 7px 8px; font-weight: 600;
        }
        QToolButton#groupButton:hover { background: #263349; }
        QToolButton#groupButton:focus { border-color: #86a7eb; }
        QToolButton#groupButton:checked { color: #c9dcff; }
        QToolButton#groupAdd { padding: 0; }
        QTabBar::tab {
            background: #1a2230; color: #b6c2d8;
            padding: 6px 9px; border: 1px solid transparent;
        }
        QTabBar::tab:selected {
            background: #29374e; color: #f4f7ff;
            border: 1px solid #3c4e6a;
        }
        QTabBar::tab:hover:!selected { background: #232f44; }
        QTabBar#projectTabs::tab { padding: 7px 9px; }
        QTabWidget#projectViews QTabBar::tab { padding: 7px 12px; }
        QTreeView#directoryTree::item { padding: 3px 4px; }
        QTreeView#directoryTree::item:hover:!selected { background: #202c40; }
        QHeaderView::section {
            background: #263146; border: 0; border-right: 1px solid palette(mid); padding: 4px 6px;
        }
        QTreeView, QListView, QPlainTextEdit {
            border: 1px solid palette(mid);
            selection-background-color: palette(highlight); selection-color: palette(highlighted-text);
        }
    )qss");
    if (!light)
        return sheet;

    struct ColorPair { const char *dark; const char *light; };
    static constexpr ColorPair colors[]{
        {"#111721", "#f7f9fc"},
        {"#151b25", "#f0f4f9"},
        {"#1a2230", "#edf2f8"},
        {"#1b2331", "#f7f9fc"},
        {"#222c3d", "#edf2f8"},
        {"#273348", "#f9fbfd"},
        {"#354563", "#e4edf8"},
        {"#86a7eb", "#446eae"},
        {"#1b2538", "#d5e2f1"},
        {"#222a38", "#edf1f6"},
        {"#a9c6ff", "#c9defd"},
        {"#16243f", "#192d52"},
        {"#b9d1ff", "#acc7ed"},
        {"#c5d8ff", "#b7d2f8"},
        {"#26364f", "#dbe7fd"},
        {"#3c5273", "#b2c7e5"},
        {"#263349", "#e2ebf7"},
        {"#c9dcff", "#274e85"},
        {"#b6c2d8", "#4b5f7a"},
        {"#29374e", "#fff"},
        {"#f4f7ff", "#192d52"},
        {"#3c4e6a", "#c7d6ea"},
        {"#232f44", "#e2ebf7"},
        {"#202c40", "#e9f1fc"},
        {"#263146", "#edf2f8"},
        {"#b4c9ee", "#243e64"},
    };
    for (const auto &color : colors)
        sheet.replace(QLatin1String(color.dark), QLatin1String(color.light));
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
    app.setApplicationVersion(QStringLiteral("0.1.0"));
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
