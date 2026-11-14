#include "Workspace.h"

#include "Appearance.h"
#include "CommitDiffView.h"
#include "CurrentChangesPanel.h"
#include "FilesPanel.h"
#include "GitPanel.h"
#include "RunConfigurations.h"
#include "Terminal.h"
#include "TextFileEditor.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QStyle>
#include <QSyntaxHighlighter>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <functional>
#include <utility>

#include <signal.h>
#include <unistd.h>

namespace {
const QStringList GroupNames{QStringLiteral("C++"), QStringLiteral("Python"), QStringLiteral("Web")};

QString sessionFile()
{
    return Workspace::stateDirectory() + "/workspace.json";
}

QString projectName(const QString &root)
{
    const auto name = QDir(root).dirName();
    return name.isEmpty() ? root : name;
}

QLabel *label(const QString &text, const QString &role, QWidget *parent)
{
    auto *result = new QLabel(text, parent);
    result->setObjectName(role);
    return result;
}

QToolButton *toolButton(const QString &text, const QString &tooltip, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tooltip);
    button->setAccessibleName(tooltip);
    return button;
}

class LogHighlighter final : public QSyntaxHighlighter
{
public:
    explicit LogHighlighter(QPlainTextEdit *editor)
        : QSyntaxHighlighter(editor->document()), editor_(editor)
    {
        editor_->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == editor_ && event->type() == QEvent::PaletteChange)
            rehighlight();
        return QSyntaxHighlighter::eventFilter(watched, event);
    }

    void highlightBlock(const QString &line) override
    {
        static const QRegularExpression level(
            QStringLiteral(R"(\b(TRACE|DEBUG|INFO|NOTICE|SUCCESS|WARN(?:ING)?|ERROR|CRITICAL|FATAL)\b|^([IWEF])\d{4}\s)"),
            QRegularExpression::CaseInsensitiveOption);
        const auto match = level.match(line);
        if (!match.hasMatch())
            return;

        const auto name = match.captured(1).toUpper();
        const auto prefix = match.captured(2).toUpper();
        const bool dark = editor_->palette().color(QPalette::Base).lightness() < 128;
        QColor color(dark ? "#8fc5ff" : "#245aa0");
        if (name == "TRACE" || name == "DEBUG")
            color = QColor(dark ? "#a2acbf" : "#56637a");
        else if (name == "SUCCESS")
            color = QColor(dark ? "#8de5a6" : "#216a43");
        else if (name == "WARN" || name == "WARNING" || prefix == "W")
            color = QColor(dark ? "#f3cb78" : "#815300");
        else if (name == "ERROR" || name == "CRITICAL" || name == "FATAL" || prefix == "E" || prefix == "F")
            color = QColor(dark ? "#ff9c9c" : "#a4273b");

        QTextCharFormat format;
        format.setForeground(color);
        setFormat(0, line.size(), format);
    }

private:
    QPlainTextEdit *editor_;
};

class RunOutput final : public QWidget
{
public:
    RunOutput(std::function<void(bool)> runningChanged, QWidget *parent)
        : QWidget(parent)
        , runningChanged_(std::move(runningChanged))
    {
        setObjectName("runTab");
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto *bar = new QWidget(this);
        bar->setObjectName("runBar");
        auto *barLayout = new QHBoxLayout(bar);
        barLayout->setContentsMargins(12, 4, 8, 4);
        status_ = label(tr("Running"), "muted", bar);
        status_->setTextFormat(Qt::PlainText);
        barLayout->addWidget(status_, 1);
        layout->addWidget(bar);

        output_ = new QPlainTextEdit(this);
        output_->setObjectName("runOutput");
        output_->setAccessibleName(tr("Run output"));
        output_->setReadOnly(true);
        output_->setUndoRedoEnabled(false);
        output_->setMaximumBlockCount(2500);
        output_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        new LogHighlighter(output_);
        layout->addWidget(output_, 1);

        process_.setProcessChannelMode(QProcess::MergedChannels);
        process_.setStandardInputFile(QProcess::nullDevice());
        process_.setChildProcessModifier([] {
            if (::setpgid(0, 0) == -1)
                ::_exit(126);
        });
        connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { readOutput(); });
        connect(&process_, &QProcess::started, this, [this] { status_->setText(tr("Running")); });
        connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                status_->setText(tr("Failed to start"));
                append(process_.errorString());
                runningChanged_(false);
            }
        });
        connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            readOutput();
            status_->setText(stopping_ ? tr("Process stopped") : status == QProcess::CrashExit
                ? tr("Process crashed") : tr("Exited with code %1").arg(code));
            runningChanged_(false);
        });
    }

    ~RunOutput() override
    {
        disconnect(&process_, nullptr, this, nullptr);
        if (process_.state() != QProcess::NotRunning) {
            stop();
            process_.waitForFinished(300);
        }
    }

    void start(const QString &program, const QStringList &arguments, const QString &directory,
               const QProcessEnvironment &environment)
    {
        if (process_.state() != QProcess::NotRunning)
            return;
        decoder_.resetState();
        escape_ = Escape::Text;
        stopping_ = false;
        output_->clear();
        status_->setText(tr("Starting…"));
        process_.setWorkingDirectory(directory);
        process_.setProcessEnvironment(environment);
        runningChanged_(true);
        process_.start(program, arguments);
    }

    void stop()
    {
        if (process_.state() == QProcess::NotRunning)
            return;
        stopping_ = true;
        status_->setText(tr("Stopping…"));
        const auto pid = static_cast<pid_t>(process_.processId());
        // The launcher and Python script can both start subprocesses.
        if (pid <= 0 || ::kill(-pid, SIGKILL) == -1)
            process_.kill();
    }

private:
    enum class Escape { Text, Start, Csi, String, StringEnd };

    void readOutput()
    {
        const QString chunk = decoder_(process_.readAllStandardOutput());
        QString text;
        text.reserve(chunk.size());
        // Keep escape state between reads so split ANSI sequences do not leak into the log.
        for (const auto ch : chunk) {
            switch (escape_) {
            case Escape::Text:
                if (ch == QChar(0x1b))
                    escape_ = Escape::Start;
                else
                    text.append(ch);
                break;
            case Escape::Start:
                if (ch == QLatin1Char('['))
                    escape_ = Escape::Csi;
                else if (ch == QLatin1Char(']') || ch == QLatin1Char('P') ||
                         ch == QLatin1Char('_') || ch == QLatin1Char('^'))
                    escape_ = Escape::String;
                else
                    escape_ = Escape::Text;
                break;
            case Escape::Csi:
                if (ch.unicode() >= 0x40 && ch.unicode() <= 0x7e)
                    escape_ = Escape::Text;
                break;
            case Escape::String:
                if (ch == QChar(0x07))
                    escape_ = Escape::Text;
                else if (ch == QChar(0x1b))
                    escape_ = Escape::StringEnd;
                break;
            case Escape::StringEnd:
                escape_ = ch == QLatin1Char('\\') ? Escape::Text : Escape::String;
                break;
            }
        }
        if (!text.isEmpty())
            append(text);
    }

    void append(const QString &value)
    {
        auto *bar = output_->verticalScrollBar();
        const bool follow = bar->value() == bar->maximum();
        const auto selected = output_->textCursor();
        const int scroll = bar->value();
        QTextCursor end(output_->document());
        end.movePosition(QTextCursor::End);
        // Preserve a bounded tail even if a process prints one unbroken line.
        constexpr qsizetype limit = 500'000;
        if (end.position() + value.size() > limit) {
            end.setPosition(int(std::min<qsizetype>(end.position(), end.position() + value.size() - limit)));
            end.movePosition(QTextCursor::Start, QTextCursor::KeepAnchor);
            end.removeSelectedText();
            end.movePosition(QTextCursor::End);
        }
        end.insertText(value.right(limit));
        if (follow) {
            bar->setValue(bar->maximum());
        } else {
            output_->setTextCursor(selected);
            bar->setValue(scroll);
        }
    }

    QProcess process_;
    QStringDecoder decoder_{QStringDecoder::Utf8};
    std::function<void(bool)> runningChanged_;
    Escape escape_ = Escape::Text;
    bool stopping_ = false;
    QLabel *status_ = nullptr;
    QPlainTextEdit *output_ = nullptr;
};
}

class ProjectView final : public QWidget
{
public:
    ProjectView(const QString &root, bool canRun, bool isWeb, const QJsonObject &state,
                std::function<void()> changed, QWidget *parent)
        : QWidget(parent)
        , root_(root)
        , changed_(std::move(changed))
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 10, 12, 10);

        split_ = new QSplitter(Qt::Vertical, this);
        split_->setChildrenCollapsible(false);
        split_->setHandleWidth(8);
        layout->addWidget(split_, 1);
        auto *views = views_ = new QTabWidget(split_);
        views->setObjectName("projectViews");
        views->setDocumentMode(true);
        views->tabBar()->setDrawBase(false);
        views->tabBar()->setSelectionBehaviorOnRemove(QTabBar::SelectPreviousTab);
        auto *currentChanges = new CurrentChangesPanel(root, views);
        views->addTab(currentChanges, tr("Current Changes"));
        auto *git = new GitPanel(root, views);
        views->addTab(git, tr("Commits"));
        auto *files = files_ = new FilesPanel(root, views);
        views->addTab(files, tr("Files"));
        views->setTabsClosable(true);
        for (int tab = 0; tab < views->count(); ++tab) {
            for (const auto side : {QTabBar::LeftSide, QTabBar::RightSide}) {
                if (auto *button = views->tabBar()->tabButton(tab, side)) {
                    views->tabBar()->setTabButton(tab, side, nullptr);
                    button->deleteLater();
                }
            }
        }
        views->setCurrentWidget(git);
        auto *toolbar = git->toolbar();
        views->setCornerWidget(toolbar, Qt::TopRightCorner);
        connect(views, &QTabWidget::currentChanged, views, [views, git, toolbar] {
            toolbar->setVisible(views->currentWidget() == git);
        });
        const auto showDiff = [views, files](const QString &hash, const QString &title, auto createPage) {
            const int index = views->indexOf(files) + 1;
            if (auto *existing = views->widget(index)) {
                if (!hash.isEmpty() && existing->property("commitHash").toString() == hash) {
                    views->setCurrentWidget(existing);
                    return;
                }
                if (!CommitDiffView::confirmClose(existing))
                    return;
                views->removeTab(index);
                existing->deleteLater();
            }
            auto *page = createPage();
            const int tab = views->insertTab(index, page, tr("Changes"));
            views->setTabToolTip(tab, title);
            views->setCurrentIndex(tab);
        };
        connect(git, &GitPanel::viewChangesRequested, views,
                [views, showDiff](const QString &repository, const QString &hash, const QString &subject) {
            showDiff(hash, hash.left(12) + QStringLiteral("  ") + subject,
                     [&] { return CommitDiffView::create(views, repository, hash, subject); });
        });
        connect(currentChanges, &CurrentChangesPanel::viewChangesRequested, views,
                [views, showDiff, root](const QByteArray &path) {
            showDiff({}, tr("Current changes"), [&] { return CommitDiffView::createWorking(views, root, path); });
        });
        connect(views, &QTabWidget::tabCloseRequested, views, [views, files](int tab) {
            if (tab <= views->indexOf(files))
                return;
            auto *page = views->widget(tab);
            if (!CommitDiffView::confirmClose(page))
                return;
            views->removeTab(tab);
            page->deleteLater();
        });
        auto *saveShortcut = new QShortcut(QKeySequence::Save, views);
        saveShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(saveShortcut, &QShortcut::activated, views, [views, files] {
            if (views->currentWidget() == files)
                files->save();
            else if (auto *editor = CommitDiffView::workingEditor(views->currentWidget()))
                editor->save();
        });
        split_->addWidget(views);

        auto *bottom = new QWidget(split_);
        bottom->setObjectName("terminalPanel");
        auto *bottomLayout = new QVBoxLayout(bottom);
        bottomLayout->setContentsMargins(0, 0, 0, 0);

        terminals_ = new QTabWidget(bottom);
        terminals_->setObjectName("terminalTabs");
        terminals_->setDocumentMode(true);
        terminals_->setTabsClosable(true);
        terminals_->setUsesScrollButtons(true);
        terminals_->setElideMode(Qt::ElideRight);
        terminals_->tabBar()->setDrawBase(false);
        terminals_->tabBar()->setAccessibleName(tr("Terminal tabs"));
        auto *corner = new QWidget(terminals_);
        auto *cornerLayout = new QHBoxLayout(corner);
        cornerLayout->setContentsMargins(6, 2, 6, 2);
        auto *add = toolButton(tr("+"), tr("New terminal at project root"), corner);
        add->setObjectName("terminalAdd");
        cornerLayout->addWidget(add);
        terminals_->setCornerWidget(corner, Qt::TopRightCorner);
        bottomLayout->addWidget(terminals_, 1);
        split_->addWidget(bottom);
        split_->setStretchFactor(0, 3);
        split_->setStretchFactor(1, 2);
        const auto splitterState = QByteArray::fromBase64(state.value("splitter").toString().toLatin1());
        if (splitterState.isEmpty() || !split_->restoreState(splitterState))
            split_->setSizes({390, 265});

        const auto saved = state.value("terminals");
        if (saved.isArray()) {
            for (const auto &entry : saved.toArray()) {
                const auto tab = entry.toObject();
                appendTerminal(tab.value("directory").toString(), tab.value("title").toString());
            }
        } else {
            appendTerminal(root);
        }
        if (const int count = terminals_->count()) {
            terminals_->setCurrentIndex(std::clamp(state.value("activeTerminal").toInt(), 0, count - 1));
            startTerminal(terminals_->currentIndex(), false);
        }

        auto *newTerminal = new QAction(tr("New Terminal"), this);
        newTerminal->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
        newTerminal->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(newTerminal);
        auto createTerminal = [this] {
            QWidget *page = appendTerminal(root_);
            const int index = terminals_->indexOf(page);
            terminals_->setCurrentIndex(index);
            startTerminal(index, true);
            changed_();
        };
        connect(add, &QToolButton::clicked, this, createTerminal);
        connect(newTerminal, &QAction::triggered, this, createTerminal);
        connect(terminals_, &QTabWidget::currentChanged, this, [this](int index) {
            startTerminal(index, true);
            changed_();
        });
        connect(terminals_, &QTabWidget::tabCloseRequested, this, [this](int index) {
            auto *page = terminals_->widget(index);
            if (!page || !page->property("shellTab").toBool())
                return;
            {
                const QSignalBlocker blocked(terminals_);
                terminals_->removeTab(index);
                delete page;
            }
            startTerminal(terminals_->currentIndex(), true);
            changed_();
        });

        if (canRun) {
            runs_ = new RunConfigurations(root, isWeb, this);
            runs_->restoreState(state.value("runConfigurations").toObject());
            connect(runs_, &RunConfigurations::stateChanged, this, [this] { changed_(); });
            connect(runs_, &RunConfigurations::runRequested, this,
                    [this](const QString &program, const QStringList &args,
                           const QString &dir, const QString &title, const QProcessEnvironment &environment,
                           const QString &configurationId) {
                auto *output = runOutputs_.value(configurationId);
                const auto tabTitle = tr("Run  ·  %1").arg(title);
                if (!output) {
                    output = new RunOutput([this, configurationId](bool running) {
                        runs_->setRunning(configurationId, running);
                    }, terminals_);
                    runOutputs_.insert(configurationId, output);
                    terminals_->insertTab(0, output, tabTitle);
                    for (const auto side : {QTabBar::LeftSide, QTabBar::RightSide}) {
                        if (auto *button = terminals_->tabBar()->tabButton(0, side)) {
                            terminals_->tabBar()->setTabButton(0, side, nullptr);
                            button->deleteLater();
                        }
                    }
                } else {
                    const auto index = terminals_->indexOf(output);
                    terminals_->tabBar()->moveTab(index, 0);
                    terminals_->setTabText(0, tabTitle);
                }
                terminals_->setTabToolTip(0, title);
                terminals_->setCurrentWidget(output);
                output->start(program, args, dir, environment);
            });
            connect(runs_, &RunConfigurations::stopRequested, this, [this](const QString &configurationId) {
                if (auto *output = runOutputs_.value(configurationId)) {
                    terminals_->setCurrentWidget(output);
                    output->stop();
                }
            });
        }
    }

    RunConfigurations *runConfigurations() const { return runs_; }

    bool confirmClose()
    {
        return files_->confirmClose() && CommitDiffView::confirmClose(views_->widget(views_->indexOf(files_) + 1));
    }

    QJsonObject saveState() const
    {
        QJsonArray tabs;
        int current = 0;
        for (int i = 0; i < terminals_->count(); ++i) {
            const auto *page = terminals_->widget(i);
            if (!page->property("shellTab").toBool())
                continue;
            if (i == terminals_->currentIndex())
                current = int(tabs.size());
            QString cwd = page->property("directory").toString();
            if (const auto *terminal = terminalIn(page)) {
                const auto latest = terminal->workingDirectory();
                if (!latest.isEmpty())
                    cwd = latest;
            }
            tabs.append(QJsonObject{{"directory", cwd}, {"title", terminals_->tabText(i)}});
        }
        QJsonObject state{
            {"terminals", tabs},
            {"activeTerminal", current},
            {"splitter", QString::fromLatin1(split_->saveState().toBase64())},
        };
        if (runs_)
            state.insert("runConfigurations", runs_->saveState());
        return state;
    }

private:
    QTabWidget *views_ = nullptr;
    FilesPanel *files_ = nullptr;

    static Terminal *terminalIn(const QWidget *page)
    {
        const auto *item = page->layout()->itemAt(0);
        return item ? static_cast<Terminal *>(item->widget()) : nullptr;
    }

    QWidget *appendTerminal(const QString &directory, QString title = {})
    {
        if (title.isEmpty()) {
            for (int number = 1; title.isEmpty(); ++number) {
                const auto candidate = tr("Terminal %1").arg(number);
                bool inUse = false;
                for (int i = 0; i < terminals_->count(); ++i)
                    inUse |= terminals_->tabText(i) == candidate;
                if (!inUse)
                    title = candidate;
            }
        }
        auto *page = new QWidget(terminals_);
        page->setProperty("shellTab", true);
        page->setProperty("directory", QDir(directory).isAbsolute() ? directory : root_);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        const int index = terminals_->addTab(page, title);
        terminals_->setTabToolTip(index, page->property("directory").toString());
        return page;
    }

    void startTerminal(int index, bool focus)
    {
        if (index < 0)
            return;
        auto *page = terminals_->widget(index);
        if (!page->property("shellTab").toBool())
            return;
        auto *terminal = terminalIn(page);
        if (!terminal) {
            auto cwd = page->property("directory").toString();
            if (!QFileInfo(cwd).isDir())
                cwd = QFileInfo(root_).isDir() ? root_ : QDir::homePath();
            terminal = new Terminal(cwd, page);
            page->layout()->addWidget(terminal);
            terminals_->setTabToolTip(index, cwd);
        }
        if (focus)
            terminal->setFocus();
    }

    QString root_;
    std::function<void()> changed_;
    QSplitter *split_ = nullptr;
    QTabWidget *terminals_ = nullptr;
    QHash<QString, RunOutput *> runOutputs_;
    RunConfigurations *runs_ = nullptr;
};

QString Workspace::stateDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

Workspace::Workspace()
{
    setWindowTitle(QApplication::applicationDisplayName());
    resize(1220, 810);
    setMinimumSize(830, 550);
    readSession();

    auto *shell = new QWidget(this);
    shell->setObjectName("shell");
    setCentralWidget(shell);
    auto *shellLayout = new QHBoxLayout(shell);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    shellLayout->setSpacing(0);

    auto *sidebar = new QWidget(shell);
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(172);
    auto *rail = new QVBoxLayout(sidebar);
    rail->setContentsMargins(12, 15, 12, 12);
    rail->setSpacing(6);
    rail->addWidget(label(QApplication::applicationDisplayName(), "brand", sidebar));
    rail->addSpacing(19);
    rail->addWidget(label(tr("PROJECT GROUPS"), "sectionLabel", sidebar));

    for (int i = 0; i < GroupCount; ++i) {
        auto *row = new QWidget(sidebar);
        row->setObjectName("groupRow");
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(1, 1, 4, 1);
        line->setSpacing(4);
        auto *button = toolButton(GroupNames.at(i), tr("%1 projects").arg(GroupNames.at(i)), row);
        button->setObjectName("groupButton");
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setCheckable(true);
        groups_[i] = button;
        counts_[i] = label("0", "groupCount", row);
        counts_[i]->setAlignment(Qt::AlignCenter);
        auto *addProject = toolButton(tr("+"), tr("Open project in %1").arg(GroupNames.at(i)), row);
        addProject->setObjectName("groupAdd");
        addProject->setFixedSize(26, 26);
        line->addWidget(button, 1);
        line->addWidget(counts_[i]);
        line->addWidget(addProject);
        rail->addWidget(row);
        connect(button, &QToolButton::clicked, this, [this, i] { selectGroup(Group(i)); });
        connect(addProject, &QToolButton::clicked, this, [this, i] { openProjectDialog(Group(i)); });
    }
    rail->addStretch();
    shellLayout->addWidget(sidebar);

    auto *surface = new QWidget(shell);
    surface->setObjectName("workspace");
    auto *surfaceLayout = new QVBoxLayout(surface);
    surfaceLayout->setContentsMargins(0, 0, 0, 0);
    surfaceLayout->setSpacing(0);
    shellLayout->addWidget(surface, 1);
    auto *tabStrip = new QWidget(surface);
    tabStrip->setObjectName("projectStrip");
    auto *stripLayout = new QHBoxLayout(tabStrip);
    stripLayout->setContentsMargins(8, 4, 9, 4);
    stripLayout->setSpacing(10);
    tabs_ = new QTabBar(tabStrip);
    tabs_->setObjectName("projectTabs");
    tabs_->setAccessibleName(tr("Project tabs"));
    tabs_->setDocumentMode(true);
    tabs_->setDrawBase(false);
    tabs_->setExpanding(false);
    tabs_->setMovable(true);
    tabs_->setTabsClosable(true);
    tabs_->setUsesScrollButtons(true);
    tabs_->setElideMode(Qt::ElideRight);
    stripLayout->addWidget(tabs_, 1, Qt::AlignVCenter);
    configurationStack_ = new QStackedWidget(tabStrip);
    configurationStack_->setObjectName("configurationStack");
    configurationStack_->hide();
    stripLayout->addWidget(configurationStack_, 0, Qt::AlignVCenter);
    surfaceLayout->addWidget(tabStrip);
    connect(tabs_, &QTabBar::currentChanged, this, [this](int tab) {
        activateTab(tab);
        updateSessionStatus(saveSession());
    });
    connect(tabs_, &QTabBar::tabCloseRequested, this, &Workspace::removeProject);
    connect(tabs_, &QTabBar::tabMoved, this, &Workspace::moveProjectTab);

    stack_ = new QStackedWidget(surface);
    surfaceLayout->addWidget(stack_, 1);
    empty_ = new QWidget(stack_);
    auto *emptyLayout = new QVBoxLayout(empty_);
    emptyLayout->setAlignment(Qt::AlignCenter);
    emptyLayout->setSpacing(11);
    emptyTitle_ = label({}, "emptyTitle", empty_);
    emptyTitle_->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyTitle_);
    auto *emptyBody = label(tr("Choose a project folder to open its Git history and terminal."), "muted", empty_);
    emptyBody->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyBody);
    emptyLayout->addSpacing(6);
    auto *emptyOpen = new QPushButton(tr("Open Project…"), empty_);
    emptyOpen->setObjectName("primary");
    connect(emptyOpen, &QPushButton::clicked, this, [this] { openProjectDialog(group_); });
    emptyLayout->addWidget(emptyOpen, 0, Qt::AlignHCenter);
    stack_->addWidget(empty_);

    auto *footer = new QWidget(surface);
    footer->setObjectName("footer");
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(13, 5, 12, 5);
    activePath_ = label({}, "muted", footer);
    activePath_->setTextFormat(Qt::PlainText);
    activePath_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    activePath_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    footerLayout->addWidget(activePath_, 1);
    sessionStatus_ = label(tr("Local workspace"), "sessionStatus", footer);
    footerLayout->addWidget(sessionStatus_);
    surfaceLayout->addWidget(footer);

    auto *openAction = new QAction(tr("Open Project"), this);
    openAction->setShortcut(QKeySequence::Open);
    addAction(openAction);
    connect(openAction, &QAction::triggered, this, [this] { openProjectDialog(group_); });

    auto *settings = new QAction(tr("Theme…"), this);
    settings->setObjectName("settingsAction");
    settings->setMenuRole(QAction::NoRole);
    settings->setShortcut(QKeySequence::Preferences);
    connect(settings, &QAction::triggered, this, &Workspace::openSettings);

    menuBar()->setNativeMenuBar(true);
    auto *settingsMenu = menuBar()->addMenu(tr("Settings"));
    settingsMenu->addAction(settings);

    rebuildTabs();
    auto *save = new QTimer(this);
    save->setInterval(5000);
    connect(save, &QTimer::timeout, this, [this] { updateSessionStatus(saveSession()); });
    save->start();
}

void Workspace::openSettings()
{
    QDialog dialog(this);
    dialog.setObjectName("settingsDialog");
    dialog.setWindowTitle(tr("Settings"));
    dialog.setMinimumWidth(300);
    auto *layout = new QFormLayout(&dialog);

    auto *theme = new QComboBox(&dialog);
    theme->setObjectName("themeSelect");
    theme->setAccessibleName(tr("Theme"));
    theme->addItem(tr("Light"), static_cast<int>(Appearance::Theme::Light));
    theme->addItem(tr("Dark"), static_cast<int>(Appearance::Theme::Dark));
    theme->setCurrentIndex(theme->findData(static_cast<int>(Appearance::theme())));
    layout->addRow(tr("Theme"), theme);
    connect(theme, &QComboBox::currentIndexChanged, &dialog, [theme] {
        Appearance::setTheme(static_cast<Appearance::Theme>(theme->currentData().toInt()));
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    dialog.exec();
}

void Workspace::openProjectDialog(Group group)
{
    const auto selected = QFileDialog::getExistingDirectory(
        this, tr("Open Project in %1").arg(GroupNames.at(group)), QDir::homePath());
    if (selected.isEmpty())
        return;
    const QString root = QFileInfo(selected).canonicalFilePath();
    if (root.isEmpty()) {
        QMessageBox::warning(this, tr("Open Project"), tr("The selected folder is unavailable."));
        return;
    }
    for (const auto &project : std::as_const(projects_)) {
        if (project.root == root) {
            lastProject_[project.group] = root;
            selectGroup(project.group);
            return;
        }
    }

    auto savedGroups = closedRunConfigurations_.value(root).toObject();
    const auto runs = savedGroups.take(QString::number(group)).toObject();
    QJsonObject state;
    if (!runs.isEmpty()) {
        state.insert("runConfigurations", runs);
        if (savedGroups.isEmpty())
            closedRunConfigurations_.remove(root);
        else
            closedRunConfigurations_.insert(root, savedGroups);
    }
    projects_.append({root, group, state, nullptr});
    lastProject_[group] = root;
    selectGroup(group);
}

void Workspace::selectGroup(Group group)
{
    group_ = group;
    rebuildTabs();
    updateSessionStatus(saveSession());
}

void Workspace::rebuildTabs()
{
    int counts[GroupCount]{};
    int current = 0;
    {
        const QSignalBlocker blocked(tabs_);
        while (tabs_->count())
            tabs_->removeTab(0);
        for (int i = 0; i < projects_.size(); ++i) {
            const auto &project = projects_.at(i);
            ++counts[project.group];
            if (group_ != project.group)
                continue;
            const int tab = tabs_->addTab(projectName(project.root));
            tabs_->setTabData(tab, i);
            tabs_->setTabToolTip(tab, project.root);
            if (project.root == lastProject_[group_])
                current = tab;
        }
        for (int i = 0; i < GroupCount; ++i) {
            const bool active = group_ == i;
            groups_[i]->setChecked(active);
            auto *row = groups_[i]->parentWidget();
            if (row->property("active") != active) {
                row->setProperty("active", active);
                row->style()->unpolish(row);
                row->style()->polish(row);
                row->update();
            }
            counts_[i]->setText(QString::number(counts[i]));
        }
        tabs_->setCurrentIndex(current);
    }
    activateTab(tabs_->currentIndex());
}

void Workspace::activateTab(int tab)
{
    if (tab < 0 || tab >= tabs_->count()) {
        configurationStack_->hide();
        emptyTitle_->setText(tr("No %1 projects open").arg(GroupNames.at(group_)));
        stack_->setCurrentWidget(empty_);
        activePath_->clear();
        activePath_->setToolTip({});
        return;
    }
    auto &project = projects_[tabs_->tabData(tab).toInt()];
    if (!project.view) {
        project.view = new ProjectView(project.root, project.group != Cpp, project.group == Web, project.state,
                                      [this] { updateSessionStatus(saveSession()); }, stack_);
        stack_->addWidget(project.view);
        if (auto *runs = project.view->runConfigurations())
            configurationStack_->addWidget(runs);
    }
    stack_->setCurrentWidget(project.view);
    if (auto *runs = project.view->runConfigurations()) {
        configurationStack_->setCurrentWidget(runs);
        configurationStack_->show();
    } else {
        configurationStack_->hide();
    }
    activePath_->setText(QDir::toNativeSeparators(project.root));
    activePath_->setToolTip(project.root);
    lastProject_[group_] = project.root;
}

void Workspace::moveProjectTab(int from, int to)
{
    // Qt has already moved the tabs, but their data still holds the old project indices.
    const int source = tabs_->tabData(to).toInt();
    const int destination = tabs_->tabData(to + (from < to ? -1 : 1)).toInt();
    projects_.move(source, destination);

    const auto [first, last] = std::minmax(source, destination);
    const int shift = source < destination ? -1 : 1;
    for (int tab = 0; tab < tabs_->count(); ++tab) {
        const int index = tabs_->tabData(tab).toInt();
        if (index == source)
            tabs_->setTabData(tab, destination);
        else if (index >= first && index <= last)
            tabs_->setTabData(tab, index + shift);
    }
    updateSessionStatus(saveSession());
}

void Workspace::removeProject(int tab)
{
    const int index = tabs_->tabData(tab).toInt();
    const auto project = projects_.at(index);
    if (project.view && !project.view->confirmClose())
        return;
    auto *runs = project.view ? project.view->runConfigurations() : nullptr;
    const auto runState = runs ? runs->saveState() : project.state.value("runConfigurations").toObject();
    if (!runState.value("configurations").toArray().isEmpty()) {
        auto savedGroups = closedRunConfigurations_.value(project.root).toObject();
        savedGroups.insert(QString::number(project.group), runState);
        closedRunConfigurations_.insert(project.root, savedGroups);
    }
    if (project.view) {
        delete runs;
        stack_->removeWidget(project.view);
        delete project.view;
    }
    projects_.removeAt(index);
    if (lastProject_[project.group] == project.root)
        lastProject_[project.group].clear();
    rebuildTabs();
    updateSessionStatus(saveSession());
}

void Workspace::readSession()
{
    QFile file(sessionFile());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const auto data = QJsonDocument::fromJson(file.readAll()).object();
    closedRunConfigurations_ = data.value("closedRunConfigurations").toObject();
    group_ = Group(std::clamp(data.value("group").toInt(), 0, int(GroupCount) - 1));
    const auto selected = data.value("selected").toArray();
    for (int i = 0; i < std::min<qsizetype>(selected.size(), GroupCount); ++i)
        lastProject_[i] = selected.at(i).toString();
    for (const auto &item : data.value("projects").toArray()) {
        const auto project = item.toObject();
        const auto root = project.value("root").toString();
        const int group = project.value("group").toInt(-1);
        if (QDir::isAbsolutePath(root) && group >= 0 && group < GroupCount) {
            projects_.append({root, Group(group), project.value("state").toObject(), nullptr});
        }
    }
    restoreGeometry(QByteArray::fromBase64(data.value("geometry").toString().toLatin1()));
}

bool Workspace::saveSession()
{
    const auto path = sessionFile();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QJsonArray projects;
    for (const auto &project : std::as_const(projects_)) {
        projects.append(QJsonObject{
            {"root", project.root},
            {"group", int(project.group)},
            {"state", project.view ? project.view->saveState() : project.state},
        });
    }
    QJsonArray selected;
    for (const auto &root : lastProject_)
        selected.append(root);
    const auto document = QJsonDocument(QJsonObject{
        {"group", int(group_)},
        {"selected", selected},
        {"projects", projects},
        {"closedRunConfigurations", closedRunConfigurations_},
        {"geometry", QString::fromLatin1(saveGeometry().toBase64())},
    });
    const auto bytes = document.toJson(QJsonDocument::Compact);
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == bytes)
        return true;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

void Workspace::updateSessionStatus(bool saved)
{
    sessionStatus_->setText(saved ? tr("Local workspace") : tr("Workspace could not be saved"));
    sessionStatus_->setToolTip(saved ? QString{} : sessionFile());
}

void Workspace::closeEvent(QCloseEvent *event)
{
    for (const auto &project : std::as_const(projects_)) {
        if (project.view && !project.view->confirmClose()) {
            event->ignore();
            return;
        }
    }
    if (!saveSession()) {
        const auto decision = QMessageBox::warning(this, tr("Workspace Save Failed"),
            tr("Could not save the workspace to %1. Close anyway?").arg(sessionFile()),
            QMessageBox::Close | QMessageBox::Cancel, QMessageBox::Cancel);
        if (decision == QMessageBox::Cancel) {
            event->ignore();
            return;
        }
    }
    QMainWindow::closeEvent(event);
}
