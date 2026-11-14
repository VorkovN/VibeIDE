#include "GitActions.h"

#include "GitFileList.h"
#include "GitStatus.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <utility>

#include <signal.h>
#include <unistd.h>

namespace {
constexpr qsizetype structuredLimit = 2 * 1024 * 1024;
constexpr qsizetype diagnosticLimit = 64 * 1024;

using GitStatus::Change;
using GitStatus::displayPath;

QList<Change> workingChanges(const QByteArray &status)
{
    auto changes = GitStatus::parse(status);
    changes.removeIf([](const Change &change) { return change.status.at(1) == ' '; });
    for (auto &change : changes) {
        // An already staged rename needs only its destination staged again.
        if (change.status.at(1) != 'R')
            change.previousPath.clear();
    }
    return changes;
}

QList<Change> stagedChanges(const QByteArray &raw)
{
    const auto records = raw.split('\0');
    QList<Change> changes;
    for (qsizetype i = 0; i + 1 < records.size(); ++i) {
        const auto &entry = records.at(i);
        if (!entry.startsWith(':'))
            continue;
        const auto status = entry.mid(entry.lastIndexOf(' ') + 1);
        const auto first = records.at(++i);
        if ((status.startsWith('R') || status.startsWith('C')) && i + 1 < records.size()) {
            changes.append({records.at(++i), first, status});
        } else {
            changes.append({first, {}, status});
        }
    }
    return changes;
}

QStringList stagedCommand()
{
    return {QStringLiteral("diff"), QStringLiteral("--cached"), QStringLiteral("--raw"),
            QStringLiteral("-z"), QStringLiteral("--no-abbrev"), QStringLiteral("--find-renames"),
            QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"), QStringLiteral("--no-color"),
            QStringLiteral("--no-relative"), QStringLiteral("--ignore-submodules=none"), QStringLiteral("--")};
}

QVBoxLayout *setupDialog(QDialog &dialog, const QString &title, const QString &instruction)
{
    dialog.setWindowTitle(title);
    dialog.resize(590, 405);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel(instruction, &dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    return layout;
}

QTreeWidget *changeList(const QList<Change> &changes, QDialog &dialog, bool checkable)
{
    auto *tree = new QTreeWidget(&dialog);
    tree->setHeaderLabels({QObject::tr("File")});
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setAlternatingRowColors(true);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    GitFileList::useStatusColors(*tree);
    QList<QTreeWidgetItem *> items;
    items.reserve(changes.size());
    for (const auto &change : changes) {
        auto path = displayPath(change.path);
        if (!change.previousPath.isEmpty())
            path = displayPath(change.previousPath) + QStringLiteral(" → ") + path;
        auto *item = new QTreeWidgetItem({path});
        if (checkable)
            item->setCheckState(0, Qt::Unchecked);
        GitFileList::setStatus(*item, *tree, change.status);
        items.append(item);
    }
    tree->addTopLevelItems(items);
    return tree;
}

QDialogButtonBox *dialogButtons(QDialog &dialog, const QString &action, QPushButton *&accept)
{
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    accept = buttons->addButton(action, QDialogButtonBox::AcceptRole);
    accept->setEnabled(false);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return buttons;
}

void appendTail(QByteArray &buffer, const QByteArray &chunk, qsizetype limit)
{
    const auto trim = buffer.size() + chunk.size() - limit;
    if (trim >= buffer.size()) {
        buffer = chunk.right(limit);
    } else {
        if (trim > 0)
            buffer.remove(0, trim);
        buffer.append(chunk);
    }
}

bool isCommitHash(const QString &hash)
{
    static const QRegularExpression objectId(QStringLiteral("\\A(?:[0-9a-f]{40}|[0-9a-f]{64})\\z"));
    return objectId.match(hash).hasMatch();
}
}

GitActions::GitActions(const QString &projectRoot, QWidget *parent)
    : QWidget(parent)
    , projectRoot_(projectRoot)
    , fetch_(new QPushButton(tr("Fetch"), this))
    , add_(new QPushButton(tr("Add"), this))
    , commit_(new QPushButton(tr("Commit"), this))
{
    setObjectName("gitActions");
    fetch_->setObjectName("gitFetch");
    add_->setObjectName("gitAdd");
    commit_->setObjectName("gitCommit");

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    for (auto *button : {fetch_, add_, commit_})
        layout->addWidget(button);

    fetch_->setToolTip(tr("Fetch from the default Git remote"));
    add_->setToolTip(tr("Stage selected working tree changes"));
    commit_->setToolTip(tr("Commit staged changes locally"));
    connect(fetch_, &QPushButton::clicked, this, &GitActions::fetch);
    connect(add_, &QPushButton::clicked, this, &GitActions::add);
    connect(commit_, &QPushButton::clicked, this, &GitActions::commit);
}

GitActions::~GitActions()
{
    if (process_ && process_->state() != QProcess::NotRunning) {
        process_->disconnect(this);
        const auto pid = static_cast<pid_t>(process_->processId());
        if (pid <= 0 || ::kill(-pid, SIGKILL) == -1)
            process_->kill();
        process_->waitForFinished(300);
    }
}

void GitActions::checkoutCommit(const QString &hash)
{
    if (process_ || !isCommitHash(hash))
        return;
    if (QMessageBox::question(this, tr("Checkout Commit"),
            tr("Check out %1 in detached HEAD mode? Your current branch pointer will stay where it is.")
                .arg(hash.left(12)),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    run({QStringLiteral("checkout"), QStringLiteral("--detach"), hash},
        [this](Result result) { completeAction(tr("Checkout"), nullptr, std::move(result)); });
}

void GitActions::cherryPickCommit(const QString &hash)
{
    if (process_ || !isCommitHash(hash))
        return;
    if (QMessageBox::question(this, tr("Cherry-pick Commit"),
            tr("Apply the changes from %1 onto the current HEAD? This creates a local commit if Git can apply them.")
                .arg(hash.left(12)),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    run({QStringLiteral("cherry-pick"), hash},
        [this](Result result) { completeAction(tr("Cherry-pick"), nullptr, std::move(result)); });
}

void GitActions::fetch()
{
    run({QStringLiteral("remote")}, [this](Result result) {
        if (!result.error.isEmpty()) {
            showError(tr("Fetch"), result.error);
            return;
        }
        if (result.output.trimmed().isEmpty()) {
            showError(tr("Fetch"), tr("This repository has no configured Git remote."));
            return;
        }
        run({QStringLiteral("fetch")}, [this](Result fetched) {
            completeAction(tr("Fetch"), fetch_, std::move(fetched));
        });
    }, {}, true);
}

void GitActions::add()
{
    run(GitStatus::arguments(), [this](Result result) {
        if (!result.error.isEmpty()) {
            showError(tr("Add"), result.error);
            return;
        }
        const auto changes = workingChanges(result.output);
        if (changes.isEmpty()) {
            QMessageBox::information(this, tr("Stage Files"), tr("There are no unstaged changes."));
            return;
        }

        QDialog dialog(this);
        auto *layout = setupDialog(dialog, tr("Stage Files"), tr("Select the files to add to the Git index."));
        auto *files = changeList(changes, dialog, true);
        files->setObjectName("gitStageFiles");
        layout->addWidget(files, 1);

        QPushButton *stage;
        auto *buttons = dialogButtons(dialog, tr("Stage"), stage);
        auto *selectAll = buttons->addButton(tr("Select All"), QDialogButtonBox::ActionRole);
        connect(files, &QTreeWidget::itemChanged, &dialog, [files, stage] {
            for (int row = 0; row < files->topLevelItemCount(); ++row) {
                if (files->topLevelItem(row)->checkState(0) == Qt::Checked) {
                    stage->setEnabled(true);
                    return;
                }
            }
            stage->setEnabled(false);
        });
        connect(selectAll, &QPushButton::clicked, &dialog, [files, stage] {
            const QSignalBlocker blocked(files);
            for (int row = 0; row < files->topLevelItemCount(); ++row)
                files->topLevelItem(row)->setCheckState(0, Qt::Checked);
            stage->setEnabled(true);
        });
        layout->addWidget(buttons);

        if (dialog.exec() != QDialog::Accepted)
            return;
        QByteArray pathspecs;
        for (int row = 0; row < files->topLevelItemCount(); ++row) {
            if (files->topLevelItem(row)->checkState(0) != Qt::Checked)
                continue;
            const auto &change = changes.at(row);
            if (!change.previousPath.isEmpty())
                pathspecs += QByteArrayLiteral(":(top,literal)") + change.previousPath + '\0';
            pathspecs += QByteArrayLiteral(":(top,literal)") + change.path + '\0';
        }
        if (pathspecs.isEmpty())
            return;
        run({QStringLiteral("add"), QStringLiteral("-A"), QStringLiteral("--pathspec-from-file=-"),
             QStringLiteral("--pathspec-file-nul")},
            [this](Result staged) { completeAction(tr("Add"), add_, std::move(staged)); },
            std::move(pathspecs));
    }, {}, true);
}

void GitActions::commit()
{
    run(stagedCommand(), [this](Result result) {
        if (!result.error.isEmpty()) {
            showError(tr("Commit"), result.error);
            return;
        }
        const auto changes = stagedChanges(result.output);
        if (changes.isEmpty()) {
            QMessageBox::information(this, tr("Commit Staged Changes"),
                                     tr("Stage changes before committing."));
            return;
        }
        if (std::ranges::any_of(changes, [](const Change &change) { return change.status == "U"; })) {
            showError(tr("Commit"), tr("Resolve and stage conflicted files before committing."));
            return;
        }

        QDialog dialog(this);
        auto *layout = setupDialog(dialog, tr("Commit Staged Changes"),
                                   tr("These staged files will be included in the local commit."));
        layout->addWidget(changeList(changes, dialog, false), 1);
        auto *message = new QPlainTextEdit(&dialog);
        message->setObjectName("gitCommitMessage");
        message->setAccessibleName(tr("Commit message"));
        message->setPlaceholderText(tr("Commit message"));
        message->setMaximumHeight(110);
        layout->addWidget(message);
        message->setFocus();

        QPushButton *submit;
        auto *buttons = dialogButtons(dialog, tr("Commit"), submit);
        connect(message, &QPlainTextEdit::textChanged, &dialog, [message, submit] {
            submit->setEnabled(!message->toPlainText().trimmed().isEmpty());
        });
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted || message->toPlainText().trimmed().isEmpty())
            return;

        // Recheck the staged content after the message dialog, which may have been open for some time.
        run(stagedCommand(), [this, expected = std::move(result.output), text = message->toPlainText().toUtf8()]
            (Result current) {
            if (!current.error.isEmpty()) {
                showError(tr("Commit"), current.error);
                return;
            }
            if (current.output != expected) {
                showError(tr("Commit"), tr("The staged changes changed. Open the commit dialog again to review them."));
                return;
            }
            run({QStringLiteral("commit"), QStringLiteral("--file=-")},
                [this](Result committed) { completeAction(tr("Commit"), commit_, std::move(committed)); },
                text + '\n');
        }, {}, true);
    }, {}, true);
}

void GitActions::run(QStringList arguments, Completion completion, QByteArray input, bool structuredOutput)
{
    if (process_)
        return;

    auto *process = new QProcess(this);
    process_ = process;
    setBusy(true);
    process->setWorkingDirectory(projectRoot_);
    process->setChildProcessModifier([] {
        if (::setpgid(0, 0) == -1)
            ::_exit(126);
    });

    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    environment.remove(QStringLiteral("GIT_DIR"));
    environment.remove(QStringLiteral("GIT_WORK_TREE"));
    environment.remove(QStringLiteral("GIT_INDEX_FILE"));
    process->setProcessEnvironment(environment);

    struct Output {
        QByteArray data;
        QByteArray error;
        bool oversized = false;
    };
    const auto output = std::make_shared<Output>();
    auto drain = [process, output, structuredOutput] {
        const auto data = process->readAllStandardOutput();
        if (structuredOutput) {
            if (data.size() > structuredLimit - output->data.size()) {
                output->oversized = true;
                process->kill();
            } else {
                output->data += data;
            }
        } else {
            appendTail(output->data, data, diagnosticLimit);
        }
        appendTail(output->error, process->readAllStandardError(), diagnosticLimit);
    };
    connect(process, &QProcess::readyReadStandardOutput, this, drain);
    connect(process, &QProcess::readyReadStandardError, this, drain);

    auto finish = [this, process, completion = std::move(completion)](Result result) {
        process_ = nullptr;
        process->deleteLater();
        setBusy(false);
        completion(std::move(result));
    };
    connect(process, &QProcess::finished, this,
            [output, drain, finish](int code, QProcess::ExitStatus status) {
        drain();
        QString error;
        if (output->oversized) {
            error = tr("Git returned too many files to display.");
        } else if (code != 0 || status != QProcess::NormalExit) {
            error = QString::fromUtf8(output->error).trimmed();
            if (error.isEmpty())
                error = QString::fromUtf8(output->data).trimmed();
            if (error.isEmpty())
                error = tr("Git exited with code %1.").arg(code);
        }
        finish({std::move(output->data), std::move(error)});
    });
    connect(process, &QProcess::errorOccurred, this, [process, finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish({{}, tr("Git could not start. %1").arg(process->errorString())});
    });

    arguments.prepend(QStringLiteral("--no-pager"));
    if (!input.isEmpty()) {
        connect(process, &QProcess::started, this, [process, input = std::move(input)] {
            process->write(input);
            process->closeWriteChannel();
        });
        process->start(QStringLiteral("git"), arguments);
    } else {
        process->setStandardInputFile(QProcess::nullDevice());
        process->start(QStringLiteral("git"), arguments, QIODevice::ReadOnly);
    }
}

void GitActions::setBusy(bool busy)
{
    for (auto *button : {fetch_, add_, commit_})
        button->setEnabled(!busy);
}

void GitActions::showError(const QString &action, const QString &error)
{
    QMessageBox message(QMessageBox::Warning, tr("Git %1").arg(action), error, QMessageBox::Ok, this);
    message.setTextFormat(Qt::PlainText);
    message.setTextInteractionFlags(Qt::TextSelectableByMouse);
    message.exec();
}

void GitActions::completeAction(const QString &action, QPushButton *button, Result result)
{
    if (!result.error.isEmpty()) {
        showError(action, result.error);
        emit repositoryChanged();
        return;
    }
    if (button)
        button->setToolTip(tr("Git %1 completed").arg(action));
    emit repositoryChanged();
}
