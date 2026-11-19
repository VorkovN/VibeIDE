#include "RunConfigurations.h"

#include "PythonLaunch.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStyle>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {
constexpr qsizetype recentLimit = 4;

QString uniqueId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString uvExecutable()
{
    const auto onPath = QStandardPaths::findExecutable(QStringLiteral("uv"));
    if (!onPath.isEmpty())
        return onPath;
    const auto userInstall = QDir::home().filePath(QStringLiteral(".local/bin/uv"));
    const QFileInfo executable(userInstall);
    return executable.isFile() && executable.isExecutable() ? userInstall : QString();
}

bool applyEnvironment(const QStringList &entries, QProcessEnvironment &environment)
{
    for (const auto &entry : entries) {
        if (entry.trimmed().isEmpty())
            continue;
        const auto separator = entry.indexOf(QLatin1Char('='));
        if (separator < 1 || entry.contains(QChar::Null))
            return false;
        const auto name = entry.left(separator).trimmed();
        if (name.isEmpty() || std::ranges::any_of(name, [](QChar c) { return c.isSpace(); }))
            return false;
        environment.insert(name, entry.mid(separator + 1));
    }
    return true;
}
}

RunConfigurations::RunConfigurations(const QString &projectRoot, bool isWeb, std::function<bool()> prepareRun,
                                     QWidget *parent)
    : QWidget(parent),
      projectRoot_(projectRoot),
      isWeb_(isWeb),
      prepareRun_(std::move(prepareRun)),
      menu_(new QMenu(this)),
      selector_(new QToolButton(this)),
      runButton_(new QPushButton(tr("Run"), this))
{
    selector_->setMenu(menu_);
    selector_->setPopupMode(QToolButton::InstantPopup);
    selector_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    selector_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    selector_->setMaximumWidth(250);
    selector_->setAccessibleName(tr("Run configurations"));
    runButton_->setObjectName(QStringLiteral("runButton"));
    runButton_->setProperty("running", false);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addWidget(selector_);
    layout->addWidget(runButton_);

    connect(menu_, &QMenu::aboutToShow, this, &RunConfigurations::rebuildMenu);
    connect(runButton_, &QPushButton::clicked, this, &RunConfigurations::runOrStopSelected);
    refreshButtons();
}

QJsonObject RunConfigurations::saveState() const
{
    QJsonArray configs;
    for (const auto &config : configurations_) {
        configs.append(QJsonObject{
            {QStringLiteral("id"), config.id},
            {QStringLiteral("name"), config.name},
            {QStringLiteral("kind"), static_cast<int>(config.kind)},
            {QStringLiteral("program"), config.program},
            {QStringLiteral("entry"), config.entry},
            {QStringLiteral("workingDirectory"), config.workingDirectory},
            {QStringLiteral("arguments"), config.arguments},
            {QStringLiteral("useUv"), config.useUv},
            {QStringLiteral("launchMode"), config.detached ? QStringLiteral("detached") : QStringLiteral("managed")},
            {QStringLiteral("applicationName"), config.applicationName},
            {QStringLiteral("environment"), QJsonArray::fromStringList(config.environment)},
        });
    }
    return {
        {QStringLiteral("configurations"), configs},
        {QStringLiteral("recent"), QJsonArray::fromStringList(recent_)},
        {QStringLiteral("selected"), selectedId_},
    };
}

void RunConfigurations::restoreState(const QJsonObject &state)
{
    const auto configs = state.value(QStringLiteral("configurations")).toArray();
    configurations_.clear();
    QStringList ids;
    for (const auto &value : configs) {
        const auto data = value.toObject();
        const auto id = data.value(QStringLiteral("id")).toString();
        const auto name = data.value(QStringLiteral("name")).toString();
        const int kind = data.value(QStringLiteral("kind")).toInt(-1);
        const bool validKind = isWeb_ ? kind == static_cast<int>(Kind::ShellScript)
            : kind == static_cast<int>(Kind::PythonScript) || kind == static_cast<int>(Kind::PythonModule);
        if (id.isEmpty() || name.isEmpty() || ids.contains(id) || !validKind)
            continue;

        QStringList environment;
        const auto entries = data.value(QStringLiteral("environment")).toArray();
        for (const auto &entry : entries) {
            if (entry.isString())
                environment.append(entry.toString());
        }
        configurations_.append(Configuration{
            .id = id,
            .name = name,
            .kind = static_cast<Kind>(kind),
            .program = data.value(QStringLiteral("program")).toString(),
            .entry = data.value(QStringLiteral("entry")).toString(),
            .workingDirectory = data.value(QStringLiteral("workingDirectory")).toString(),
            .arguments = data.value(QStringLiteral("arguments")).toString(),
            .useUv = data.value(QStringLiteral("useUv")).toBool(),
            .detached = !isWeb_ && data.value(QStringLiteral("launchMode")).toString() == QStringLiteral("detached"),
            .applicationName = data.value(QStringLiteral("applicationName")).toString(),
            .environment = std::move(environment),
        });
        ids.append(id);
    }

    recent_.clear();
    const auto recent = state.value(QStringLiteral("recent")).toArray();
    for (const auto &value : recent) {
        const auto id = value.toString();
        if (recent_.size() == recentLimit)
            break;
        if (ids.contains(id) && !recent_.contains(id))
            recent_.append(id);
    }

    selectedId_ = state.value(QStringLiteral("selected")).toString();
    if (!ids.contains(selectedId_))
        selectedId_ = configurations_.isEmpty() ? QString() : configurations_.first().id;
    refreshButtons();
}

const RunConfigurations::Configuration *RunConfigurations::selectedConfiguration() const
{
    const auto selected = std::ranges::find(configurations_, selectedId_, &Configuration::id);
    return selected == configurations_.end() ? nullptr : &*selected;
}

QString RunConfigurations::kindName(Kind kind)
{
    switch (kind) {
    case Kind::PythonScript: return tr("Python script");
    case Kind::PythonModule: return tr("Python module");
    case Kind::ShellScript: return tr("Shell script");
    }
    return {};
}

QString RunConfigurations::projectPath(const QString &path) const
{
    return QDir::cleanPath(QDir(projectRoot_).absoluteFilePath(path));
}

QString RunConfigurations::commandPath(const QString &program) const
{
    if (program.contains(QLatin1Char('/')) || program.contains(QDir::separator()))
        return projectPath(program);
    return program;
}

void RunConfigurations::refreshButtons()
{
    const auto *config = selectedConfiguration();
    const auto label = config ? config->name : tr("Configurations");
    selector_->setText(selector_->fontMetrics().elidedText(label, Qt::ElideRight, 210));
    selector_->setToolTip(label);
    runButton_->setEnabled(config != nullptr);
    const bool running = config && runningIds_.contains(config->id);
    runButton_->setText(running ? tr("Stop") : tr("Run"));
    runButton_->setAccessibleName(running ? tr("Stop selected configuration") : tr("Run selected configuration"));
    if (runButton_->property("running").toBool() != running) {
        runButton_->setProperty("running", running);
        runButton_->style()->unpolish(runButton_);
        runButton_->style()->polish(runButton_);
        runButton_->update();
    }
}

void RunConfigurations::setRunning(const QString &configurationId, bool running)
{
    if (running)
        runningIds_.insert(configurationId);
    else
        runningIds_.remove(configurationId);
    if (configurationId == selectedId_)
        refreshButtons();
}

void RunConfigurations::rebuildMenu()
{
    menu_->clear();
    auto addHeading = [this](const QString &heading) {
        menu_->addAction(heading)->setEnabled(false);
    };
    auto addConfiguration = [this](const Configuration &config) {
        const auto id = config.id;
        auto label = config.name;
        label.replace(QLatin1Char('&'), QStringLiteral("&&"));
        auto *action = menu_->addAction(label, this, [this, id] { selectConfiguration(id); });
        action->setCheckable(true);
        action->setChecked(id == selectedId_);
    };

    if (!recent_.isEmpty()) {
        addHeading(tr("Recent Configurations"));
        for (const auto &id : recent_) {
            const auto config = std::ranges::find(configurations_, id, &Configuration::id);
            if (config != configurations_.end())
                addConfiguration(*config);
        }
        menu_->addSeparator();
    }

    if (!configurations_.isEmpty()) {
        QList<const Configuration *> sorted;
        for (const auto &configuration : configurations_)
            sorted.append(&configuration);
        std::ranges::stable_sort(sorted, [](const Configuration *lhs, const Configuration *rhs) {
            return lhs->name.localeAwareCompare(rhs->name) < 0;
        });
        addHeading(tr("All Configurations (%1)").arg(configurations_.size()));
        for (const auto *config : sorted)
            addConfiguration(*config);
        menu_->addSeparator();
    }

    menu_->addAction(tr("New Configuration…"), this, [this] { editConfiguration(true); });
    if (selectedConfiguration()) {
        menu_->addAction(tr("Edit Selected…"), this, [this] { editConfiguration(false); });
        auto *remove = menu_->addAction(tr("Remove Selected…"), this, &RunConfigurations::removeConfiguration);
        remove->setEnabled(!runningIds_.contains(selectedId_));
    }
}

void RunConfigurations::selectConfiguration(const QString &id)
{
    if (id == selectedId_)
        return;
    selectedId_ = id;
    refreshButtons();
    emit stateChanged();
}

void RunConfigurations::editConfiguration(bool create)
{
    Configuration config;
    if (create) {
        config.id = uniqueId();
        config.kind = isWeb_ ? Kind::ShellScript : Kind::PythonScript;
        config.program = isWeb_ ? QString() : QStringLiteral("python3");
    } else if (const auto *selected = selectedConfiguration()) {
        config = *selected;
    } else {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(create ? tr("New Configuration") : tr("Edit Configuration"));
    dialog.setMinimumWidth(500);
    auto *form = new QFormLayout;
    auto *name = new QLineEdit(config.name, &dialog);
    auto *entry = new QLineEdit(config.entry, &dialog);
    form->addRow(tr("Name"), name);

    QComboBox *kind = nullptr;
    QLineEdit *program = nullptr;
    QLineEdit *workingDirectory = nullptr;
    QLineEdit *arguments = nullptr;
    QCheckBox *useUv = nullptr;
    QComboBox *launchMode = nullptr;
    QLineEdit *applicationName = nullptr;
    QPlainTextEdit *environment = nullptr;
    if (isWeb_) {
        entry->setPlaceholderText(tr("Path relative to project root, or absolute path"));
        form->addRow(tr("Script"), entry);
    } else {
        kind = new QComboBox(&dialog);
        for (const auto item : {Kind::PythonScript, Kind::PythonModule})
            kind->addItem(kindName(item), static_cast<int>(item));
        kind->setCurrentIndex(kind->findData(static_cast<int>(config.kind)));
        program = new QLineEdit(config.program, &dialog);
        auto *entryLabel = new QLabel(&dialog);
        entryLabel->setBuddy(entry);

        workingDirectory = new QLineEdit(config.workingDirectory, &dialog);
        arguments = new QLineEdit(config.arguments, &dialog);
        useUv = new QCheckBox(tr("Run with uv using the existing environment"), &dialog);
        useUv->setChecked(config.useUv);
        launchMode = new QComboBox(&dialog);
        launchMode->setObjectName(QStringLiteral("pythonLaunchMode"));
        launchMode->addItem(tr("Managed by IDE"), false);
        launchMode->addItem(tr("Detached · keep running after IDE closes"), true);
        launchMode->setCurrentIndex(config.detached ? 1 : 0);
        applicationName = new QLineEdit(config.applicationName, &dialog);
        applicationName->setObjectName(QStringLiteral("pythonApplicationName"));
        applicationName->setToolTip(tr("Name in the macOS Dock. Defaults to the project and configuration names."));
        const auto updateApplicationName = [this, name, applicationName] {
            applicationName->setPlaceholderText(QDir(projectRoot_).dirName() + QStringLiteral(" · ") + name->text());
        };
        connect(name, &QLineEdit::textChanged, &dialog, updateApplicationName);
        updateApplicationName();
        environment = new QPlainTextEdit(config.environment.join(QLatin1Char('\n')), &dialog);
        environment->setPlaceholderText(tr("One NAME=value per line"));
        environment->setMaximumHeight(110);
        program->setPlaceholderText(tr("python3 or .venv/bin/python"));
        workingDirectory->setPlaceholderText(tr("Project root"));
        arguments->setPlaceholderText(tr("Use double quotes around arguments with spaces"));

        form->addRow(tr("Type"), kind);
        form->addRow(tr("Python interpreter"), program);
        form->addRow(entryLabel, entry);
        form->addRow(tr("Working directory"), workingDirectory);
        form->addRow(tr("Arguments"), arguments);
        form->addRow(tr("Launch mode"), launchMode);
        form->addRow(tr("Application name"), applicationName);
        form->addRow(useUv);
        form->addRow(tr("Environment variables"), environment);

        auto updateEntry = [kind, entry, entryLabel] {
            const bool script = kind->currentData().toInt() == static_cast<int>(Kind::PythonScript);
            entryLabel->setText(script ? tr("Script") : tr("Module"));
            entry->setPlaceholderText(script ? tr("Path relative to project root, or absolute path")
                                             : tr("e.g. package.module"));
        };
        connect(kind, &QComboBox::currentIndexChanged, &dialog, updateEntry);
        updateEntry();
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (name->text().trimmed().isEmpty() || entry->text().trimmed().isEmpty()
            || (program && program->text().trimmed().isEmpty())) {
            QMessageBox::warning(&dialog, tr("Missing details"),
                                 tr("Enter a name and fill in the fields for the selected type."));
            return;
        }
        if (environment) {
            QProcessEnvironment validation;
            if (!applyEnvironment(environment->toPlainText().split(QLatin1Char('\n')), validation)) {
                QMessageBox::warning(&dialog, tr("Invalid environment variables"), tr("Use one NAME=value per line."));
                return;
            }
        }
        dialog.accept();
    });

    if (dialog.exec() != QDialog::Accepted)
        return;

    config.name = name->text().trimmed();
    config.entry = entry->text().trimmed();
    if (!isWeb_) {
        config.kind = static_cast<Kind>(kind->currentData().toInt());
        config.program = program->text().trimmed();
        config.workingDirectory = workingDirectory->text().trimmed();
        config.arguments = arguments->text();
        config.useUv = useUv->isChecked();
        config.detached = launchMode->currentData().toBool();
        config.applicationName = applicationName->text().trimmed();
        config.environment = environment->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }

    const auto id = config.id;
    if (create) {
        configurations_.append(std::move(config));
    } else {
        const auto edited = std::ranges::find(configurations_, id, &Configuration::id);
        if (edited == configurations_.end())
            return;
        *edited = std::move(config);
    }
    selectedId_ = id;
    refreshButtons();
    emit stateChanged();
}

void RunConfigurations::removeConfiguration()
{
    const auto *selected = selectedConfiguration();
    if (!selected || runningIds_.contains(selected->id))
        return;

    const auto id = selected->id;
    if (QMessageBox::question(this, tr("Remove Configuration"),
                              tr("Remove “%1”?").arg(selected->name)) != QMessageBox::Yes)
        return;

    configurations_.removeIf([&id](const Configuration &item) { return item.id == id; });
    recent_.removeAll(id);
    selectedId_ = !recent_.isEmpty() ? recent_.first()
        : configurations_.isEmpty() ? QString() : configurations_.first().id;
    refreshButtons();
    emit stateChanged();
}

void RunConfigurations::runOrStopSelected()
{
    const auto *config = selectedConfiguration();
    if (!config)
        return;

    const auto id = config->id;
    if (runningIds_.contains(id)) {
        emit stopRequested(id);
        return;
    }
    if (!prepareRun_())
        return;

    QString program;
    QStringList arguments;
    const auto workingDirectory = isWeb_ ? projectRoot_ : projectPath(config->workingDirectory);
    if (!QFileInfo(workingDirectory).isDir()) {
        QMessageBox::warning(this, tr("Working directory unavailable"),
                             tr("The directory “%1” does not exist.").arg(workingDirectory));
        return;
    }

    auto environment = QProcessEnvironment::systemEnvironment();
    if (isWeb_) {
        program = projectPath(config->entry);
        const QFileInfo script(program);
        if (!script.isFile() || !script.isExecutable()) {
            QMessageBox::warning(this, tr("Script unavailable"),
                                 tr("The script “%1” must exist and be executable.").arg(program));
            return;
        }
    } else {
        if (!applyEnvironment(config->environment, environment)) {
            QMessageBox::warning(this, tr("Invalid environment variables"), tr("Use one NAME=value per line."));
            return;
        }

        const auto applicationName = config->applicationName.isEmpty()
            ? QDir(projectRoot_).dirName() + QStringLiteral(" · ") + config->name
            : config->applicationName;
        QString launchError;
        program = PythonLaunch::prepare(commandPath(config->program), applicationName,
                                        projectRoot_ + QLatin1Char(':') + id, environment, launchError);
        if (program.isEmpty()) {
            QMessageBox::warning(this, tr("Python launch unavailable"), launchError);
            return;
        }
        arguments = QProcess::splitCommand(config->arguments);
        if (config->kind == Kind::PythonScript) {
            arguments.prepend(projectPath(config->entry));
        } else {
            arguments.prepend(config->entry);
            arguments.prepend(QStringLiteral("-m"));
        }

        if (config->useUv) {
            const auto uv = uvExecutable();
            if (uv.isEmpty()) {
                QMessageBox::warning(this, tr("uv was not found"), tr("Install uv or add it to PATH."));
                return;
            }
            arguments = QStringList{QStringLiteral("run"), QStringLiteral("--no-sync"), QStringLiteral("--"),
                                    program} + arguments;
            program = uv;
        }
    }

    const auto title = config->name;
    const bool detached = !isWeb_ && config->detached;
    recent_.removeAll(id);
    recent_.prepend(id);
    if (recent_.size() > recentLimit)
        recent_.removeLast();
    emit stateChanged();
    emit runRequested(program, arguments, workingDirectory, title, environment, id, detached);
}
