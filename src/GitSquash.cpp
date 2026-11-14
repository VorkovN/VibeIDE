#include "GitSquash.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

#include <signal.h>
#include <unistd.h>

namespace {
constexpr qsizetype outputLimit = 64 * 1024;
constexpr auto operationName = "gitSquashOperation";

bool objectId(const QByteArray &value)
{
    static const QRegularExpression pattern(QStringLiteral("\\A(?:[0-9a-f]{40}|[0-9a-f]{64})\\z"));
    return pattern.match(QString::fromLatin1(value)).hasMatch();
}

class SquashOperation final : public QObject
{
public:
    SquashOperation(QWidget *parent, const QString &root, const QVector<GitSquash::Commit> &commits,
                    std::function<void()> completed)
        : QObject(parent)
        , window_(parent)
        , commits_(commits)
        , completed_(std::move(completed))
    {
        setObjectName(operationName);
        process_.setWorkingDirectory(root);
        process_.setChildProcessModifier([] {
            if (::setpgid(0, 0) == -1)
                ::_exit(126);
        });
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("GIT_TERMINAL_PROMPT", "0");
        for (const auto *name : {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR",
                                 "GIT_NAMESPACE", "GIT_SHALLOW_FILE"})
            environment.remove(QString::fromLatin1(name));
        environment.insert("GIT_NO_REPLACE_OBJECTS", "1");
        process_.setProcessEnvironment(environment);
        timeout_.setSingleShot(true);
        timeout_.setInterval(30000);
        connect(&timeout_, &QTimer::timeout, this, [this] {
            commandError_ = tr("The Git command timed out.");
            stopProcess();
        });
        connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { collect(); });
        connect(&process_, &QProcess::readyReadStandardError, this, [this] { collect(); });
        connect(&process_, &QProcess::started, this, [this] {
            if (!input_.isEmpty())
                process_.write(input_);
            process_.closeWriteChannel();
        });
        connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                timeout_.stop();
                fail(process_.errorString());
            }
        });
        connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            timeout_.stop();
            collect();
            if (!commandError_.isEmpty()) {
                fail(commandError_);
                return;
            }
            if (code != 0 || status != QProcess::NormalExit) {
                auto error = QString::fromUtf8(error_).trimmed();
                if (error.isEmpty())
                    error = fallbackError_;
                fail(error.isEmpty() ? tr("The Git command failed.") : error);
                return;
            }
            auto next = std::move(next_);
            next(std::move(output_));
        });
    }

    ~SquashOperation() override
    {
        disconnect(&process_, nullptr, this, nullptr);
        if (process_.state() != QProcess::NotRunning) {
            stopProcess();
            process_.waitForFinished(300);
        }
    }

    void start()
    {
        for (const auto &commit : commits_) {
            if (!objectId(commit.hash.toLatin1())) {
                fail(tr("The selection contains an invalid commit."));
                return;
            }
        }
        checkBranch([this] {
            run({"rev-list", "--parents", "--max-count=" + QString::number(commits_.size()),
                 commits_.first().hash}, [this](const QByteArray &output) {
                const auto lines = output.trimmed().split('\n');
                if (lines.size() != commits_.size()) {
                    invalidRange();
                    return;
                }
                for (qsizetype i = 0; i < lines.size(); ++i) {
                    const auto fields = lines.at(i).split(' ');
                    const bool hasNext = i + 1 < lines.size();
                    if (fields.first() != commits_.at(i).hash.toLatin1() || fields.size() > 2 ||
                        (hasNext && (fields.size() != 2 || fields.last() != commits_.at(i + 1).hash.toLatin1())) ||
                        (fields.size() == 2 && !objectId(fields.last()))) {
                        invalidRange();
                        return;
                    }
                    if (!hasNext && fields.size() == 2)
                        base_ = QString::fromLatin1(fields.last());
                }
                if (base_.isEmpty()) {
                    run({"rev-parse", "--is-shallow-repository"}, [this](const QByteArray &shallow) {
                        if (shallow.trimmed() != "false") {
                            fail(tr("This range reaches the boundary of a shallow clone. Fetch more history and try again."));
                            return;
                        }
                        ensureClean([this] { prompt(); });
                    });
                } else {
                    ensureClean([this] { prompt(); });
                }
            });
        });
    }

private:
    using Read = std::function<void(QByteArray)>;

    void stopProcess()
    {
        const auto pid = static_cast<pid_t>(process_.processId());
        if (pid <= 0 || ::kill(-pid, SIGKILL) == -1)
            process_.kill();
    }

    void run(QStringList arguments, Read next, QByteArray input = {}, QString fallback = {})
    {
        output_.clear();
        error_.clear();
        commandError_.clear();
        input_ = std::move(input);
        fallbackError_ = std::move(fallback);
        next_ = std::move(next);
        arguments.prepend("--no-pager");
        timeout_.start();
        process_.start("git", arguments);
    }

    void collect()
    {
        const auto output = process_.readAllStandardOutput();
        const auto error = process_.readAllStandardError();
        if (output.size() > outputLimit - output_.size() || error.size() > outputLimit - error_.size()) {
            if (commandError_.isEmpty())
                commandError_ = tr("Git returned too much output to verify this operation.");
            stopProcess();
            return;
        }
        output_.append(output);
        error_.append(error);
    }

    void checkBranch(std::function<void()> next)
    {
        run({"symbolic-ref", "--quiet", "HEAD"}, [this, next = std::move(next)](const QByteArray &output) {
            const QString branch = QString::fromUtf8(output.trimmed());
            if (!branch.startsWith("refs/heads/") || (!branch_.isEmpty() && branch_ != branch)) {
                fail(tr("The current branch changed. Refresh the history and select the commits again."));
                return;
            }
            branch_ = branch;
            run({"rev-parse", "--verify", "HEAD"}, [this, next](const QByteArray &head) {
                if (head.trimmed() != commits_.first().hash.toLatin1()) {
                    invalidRange();
                    return;
                }
                next();
            });
        }, {}, tr("Check out a local branch before squashing commits."));
    }

    void ensureClean(std::function<void()> next)
    {
        run({"rev-parse", "--absolute-git-dir"}, [this, next = std::move(next)](QByteArray output) {
            if (output.endsWith('\n'))
                output.chop(1);
            const auto gitDir = QString::fromUtf8(output);
            const QStringList markers{"MERGE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD", "REBASE_HEAD",
                                      "rebase-merge", "rebase-apply", "sequencer", "BISECT_START"};
            for (const auto &marker : markers) {
                if (QFileInfo::exists(gitDir + QLatin1Char('/') + marker)) {
                    fail(tr("Finish the current Git operation before squashing commits."));
                    return;
                }
            }
            run({"status", "--porcelain=v1", "-z", "--untracked-files=normal", "--ignore-submodules=none"},
                [this, next](const QByteArray &status) {
                if (!status.isEmpty()) {
                    fail(tr("The index and working tree must be clean before squashing."));
                    return;
                }
                next();
            });
        });
    }

    void prompt()
    {
        QDialog dialog(window_);
        dialog.setWindowTitle(tr("Squash Commits"));
        dialog.resize(530, 335);
        auto *layout = new QVBoxLayout(&dialog);
        auto *label = new QLabel(tr("Squash %1 commits on %2 into one? This rewrites the selected local history.")
                                     .arg(commits_.size()).arg(branch_.mid(11)), &dialog);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        layout->addWidget(label);
        auto *editor = new QPlainTextEdit(&dialog);
        editor->setObjectName("squashMessage");
        editor->setAccessibleName(tr("Squashed commit message"));
        QStringList subjects;
        for (const auto &commit : commits_)
            subjects.append(commit.subject);
        editor->setPlainText(subjects.join(QLatin1Char('\n')));
        layout->addWidget(editor, 1);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
        auto *squash = buttons->addButton(tr("Squash"), QDialogButtonBox::AcceptRole);
        const auto update = [editor, squash] {
            squash->setEnabled(!editor->toPlainText().trimmed().isEmpty());
        };
        connect(editor, &QPlainTextEdit::textChanged, &dialog, update);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        update();
        editor->setFocus();

        if (dialog.exec() != QDialog::Accepted) {
            deleteLater();
            return;
        }
        message_ = editor->toPlainText().trimmed().toUtf8() + '\n';
        checkBranch([this] { ensureClean([this] { writeCommit(); }); });
    }

    void writeCommit()
    {
        // The replacement has the exact HEAD tree, so index and worktree are untouched.
        QStringList args{"commit-tree", commits_.first().hash + "^{tree}", "-F", "-"};
        if (!base_.isEmpty())
            args << "-p" << base_;
        run(args, [this](const QByteArray &output) {
            const auto hash = output.trimmed();
            if (!objectId(hash)) {
                fail(tr("Git did not return a replacement commit ID."));
                return;
            }
            checkBranch([this, hash] {
                // Compare-and-swap preserves the old history if another process has moved this branch.
                run({"update-ref", "-m", "squash: combine selected commits", branch_,
                     QString::fromLatin1(hash), commits_.first().hash}, [this](const QByteArray &) {
                    completed_();
                    deleteLater();
                });
            });
        }, message_);
    }

    void invalidRange()
    {
        fail(tr("Select consecutive commits ending at HEAD on the current branch. The selection must contain no merge commits."));
    }

    void fail(const QString &reason)
    {
        QMessageBox dialog(QMessageBox::Warning, tr("Squash Commits"), reason, QMessageBox::Ok, window_);
        dialog.setTextFormat(Qt::PlainText);
        dialog.exec();
        deleteLater();
    }

    QWidget *window_;
    QVector<GitSquash::Commit> commits_;
    std::function<void()> completed_;
    QProcess process_;
    QTimer timeout_;
    Read next_;
    QByteArray output_;
    QByteArray error_;
    QByteArray input_;
    QByteArray message_;
    QString commandError_;
    QString fallbackError_;
    QString branch_;
    QString base_;
};
}

void GitSquash::request(QWidget *parent, const QString &root, const QVector<Commit> &commits,
                        std::function<void()> completed)
{
    if (root.isEmpty() || commits.size() < 2 || parent->findChild<QObject *>(operationName, Qt::FindDirectChildrenOnly))
        return;
    auto *operation = new SquashOperation(parent, root, commits, std::move(completed));
    operation->start();
}
