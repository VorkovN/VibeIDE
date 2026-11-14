#include "CommitDiffSource.h"

#include "GitStatus.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTemporaryDir>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr qsizetype ListLimit = 2 * 1024 * 1024;
constexpr qsizetype BlobLimit = 1 * 1024 * 1024;
constexpr qsizetype PatchLimit = 5 * 1024 * 1024;
constexpr qsizetype ErrorLimit = 8 * 1024;
constexpr qsizetype LineLimit = 50'000;

bool validObject(const QByteArray &object)
{
    if (object.size() != 40 && object.size() != 64)
        return false;
    return std::ranges::all_of(object, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool supportedMode(const QByteArray &mode)
{
    return mode == "000000" || mode == "100644" || mode == "100755" || mode == "120000";
}

bool validBlob(const QByteArray &object, const QByteArray &mode)
{
    return mode == "000000" ? object.isEmpty() : validObject(object);
}

QByteArray blobObject(const QByteArray &object)
{
    if (std::ranges::all_of(object, [](char c) { return c == '0'; }))
        return {};
    return object;
}

bool parseFiles(const QByteArray &data, QList<CommitDiffSource::File> &files)
{
    const auto records = data.split('\0');
    for (qsizetype index = 0; index < records.size(); ++index) {
        const auto &record = records.at(index);
        if (record.isEmpty() && index == records.size() - 1)
            break;
        if (!record.startsWith(':'))
            return false;
        const auto fields = record.mid(1).split(' ');
        if (fields.size() != 5 || fields[4].isEmpty() || !validObject(fields[2]) ||
            !validObject(fields[3]) || index + 1 >= records.size())
            return false;

        const auto &status = fields[4];
        const auto &path = records.at(++index);
        if (path.isEmpty())
            return false;
        CommitDiffSource::File file{
            .oldPath = fields[0] == "000000" ? QByteArray{} : path,
            .newPath = fields[1] == "000000" ? QByteArray{} : path,
            .oldObject = blobObject(fields[2]),
            .newObject = blobObject(fields[3]),
            .oldMode = fields[0],
            .newMode = fields[1],
            .status = status,
            .workingPath = {},
        };
        if (status.startsWith('R') || status.startsWith('C')) {
            if (index + 1 >= records.size())
                return false;
            file.newPath = records.at(++index);
            if (file.newPath.isEmpty())
                return false;
        }
        files.append(std::move(file));
    }
    return true;
}

QList<CommitDiffSource::File> workingFiles(const QByteArray &data)
{
    QList<CommitDiffSource::File> files;
    QHash<QByteArray, qsizetype> paths;
    for (const auto &change : GitStatus::parse(data)) {
        const bool untracked = change.status == "??";
        const bool addition = untracked || change.status.at(0) == 'A';
        const bool deletion = change.status.contains('D') && change.status != "DU";
        if (untracked) {
            const auto existing = paths.constFind(change.path);
            if (existing != paths.constEnd()) {
                auto &file = files[existing.value()];
                file.newPath = change.path;
                if (!file.oldPath.isEmpty() && file.oldPath == file.newPath)
                    file.status = QByteArrayLiteral("DM");
                continue;
            }
        }
        paths.insert(change.path, files.size());
        files.append({
            .oldPath = !change.previousPath.isEmpty() ? change.previousPath : addition ? QByteArray{} : change.path,
            .newPath = deletion ? QByteArray{} : change.path,
            .oldObject = {},
            .newObject = {},
            .oldMode = {},
            .newMode = {},
            .status = change.status,
            .workingPath = change.path,
        });
    }
    return files;
}

int lineCount(QStringView text)
{
    if (text.isEmpty())
        return 0;
    return int(text.count(QLatin1Char('\n')) + !text.endsWith(QLatin1Char('\n')));
}

QStringList hunkOptions()
{
    return {QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"),
            QStringLiteral("--no-renames"), QStringLiteral("--word-diff=none"), QStringLiteral("-U0"),
            QStringLiteral("--inter-hunk-context=0")};
}

class Descriptor final
{
public:
    explicit Descriptor(int value) : value_(value) {}
    ~Descriptor() { reset(-1); }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    int get() const { return value_; }
    void reset(int value)
    {
        if (value_ >= 0)
            ::close(value_);
        value_ = value;
    }

private:
    int value_;
};

enum class WorktreeRead { Text, Missing, Oversized, Unsupported, Changed, Error };

struct WorktreeFile {
    WorktreeRead state;
    QByteArray bytes;
    QString error = {};
};

WorktreeFile readFailure(int error)
{
    if (error == ENOENT)
        return {WorktreeRead::Missing, {}};
    if (error == ENOTDIR || error == ELOOP)
        return {WorktreeRead::Unsupported, {}};
    return {WorktreeRead::Error, {}, QString::fromLocal8Bit(std::strerror(error))};
}

bool sameSnapshot(const struct stat &before, const struct stat &after)
{
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
           before.st_mode == after.st_mode && before.st_size == after.st_size &&
           before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
           before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
           before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec &&
           before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
}

WorktreeFile readWorktreeFile(const QString &root, const QByteArray &path)
{
    if (path.isEmpty())
        return {WorktreeRead::Missing, {}};
    const auto parts = path.split('/');
    if (std::ranges::any_of(parts, [](const QByteArray &part) {
        return part.isEmpty() || part == "." || part == ".." || part.contains('\0');
    })) {
        return {WorktreeRead::Unsupported, {}};
    }

    constexpr int directoryFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    Descriptor directory(::open(QFile::encodeName(root).constData(), directoryFlags));
    if (directory.get() < 0)
        return readFailure(errno);
    for (qsizetype index = 0; index + 1 < parts.size(); ++index) {
        const int child = ::openat(directory.get(), parts.at(index).constData(), directoryFlags);
        if (child < 0)
            return readFailure(errno);
        directory.reset(child);
    }

    const auto *basename = parts.constLast().constData();
    struct stat before {};
    if (::fstatat(directory.get(), basename, &before, AT_SYMLINK_NOFOLLOW) < 0)
        return readFailure(errno);
    if (before.st_size > BlobLimit)
        return {WorktreeRead::Oversized, {}};

    if (S_ISLNK(before.st_mode)) {
        QByteArray target(std::clamp<qsizetype>(before.st_size + 1, 1024, BlobLimit + 1),
                          Qt::Uninitialized);
        const auto count = ::readlinkat(directory.get(), basename, target.data(), target.size());
        struct stat after {};
        if (count < 0 || ::fstatat(directory.get(), basename, &after, AT_SYMLINK_NOFOLLOW) < 0 ||
            !sameSnapshot(before, after))
            return {WorktreeRead::Changed, {}};
        if (count == target.size() || count > BlobLimit)
            return {WorktreeRead::Oversized, {}};
        target.truncate(count);
        return {WorktreeRead::Text, std::move(target)};
    }

    if (!S_ISREG(before.st_mode))
        return {WorktreeRead::Unsupported, {}};
    Descriptor file(::openat(directory.get(), basename, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (file.get() < 0)
        return readFailure(errno);
    struct stat opened {};
    if (::fstat(file.get(), &opened) < 0 || !sameSnapshot(before, opened))
        return {WorktreeRead::Changed, {}};
    QByteArray bytes;
    bytes.reserve(opened.st_size);
    while (bytes.size() <= BlobLimit) {
        char buffer[16384];
        const auto count = ::read(file.get(), buffer, std::min<qsizetype>(sizeof(buffer), BlobLimit + 1 - bytes.size()));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return readFailure(errno);
        }
        if (count == 0)
            break;
        bytes.append(buffer, count);
    }
    if (bytes.size() > BlobLimit)
        return {WorktreeRead::Oversized, {}};
    struct stat after {};
    if (::fstatat(directory.get(), basename, &after, AT_SYMLINK_NOFOLLOW) < 0 || !sameSnapshot(before, after) ||
        bytes.size() != after.st_size)
        return {WorktreeRead::Changed, {}};
    return {WorktreeRead::Text, std::move(bytes)};
}

void addWholeFileHunk(CommitDiffSource::Preview &preview)
{
    const int before = lineCount(preview.before);
    const int after = lineCount(preview.after);
    if (before || after)
        preview.hunks.append({before ? 1 : 0, before, after ? 1 : 0, after});
}

bool writeSnapshot(const QString &path, const QString &text)
{
    QFile file(path);
    const auto bytes = text.toUtf8();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}

QList<CommitDiffSource::Hunk> parseHunks(const QByteArray &patch)
{
    static const QRegularExpression header(QStringLiteral(
        "^@@ -([0-9]+)(?:,([0-9]+))? \\+([0-9]+)(?:,([0-9]+))? @@"));
    QList<CommitDiffSource::Hunk> hunks;
    qsizetype cursor = 0;
    while ((cursor = patch.indexOf("\n@@ ", cursor)) >= 0) {
        ++cursor;
        const auto end = patch.indexOf('\n', cursor);
        if (end < 0)
            break;
        const auto match = header.match(QString::fromLatin1(patch.constData() + cursor, end - cursor));
        cursor = end;
        if (!match.hasMatch())
            continue;
        hunks.append({
            .oldStart = match.capturedView(1).toInt(),
            .oldCount = match.hasCaptured(2) ? match.capturedView(2).toInt() : 1,
            .newStart = match.capturedView(3).toInt(),
            .newCount = match.hasCaptured(4) ? match.capturedView(4).toInt() : 1,
        });
    }
    return hunks;
}
}

CommitDiffSource::CommitDiffSource(const QString &repository, QObject *parent)
    : QObject(parent), repository_(repository)
{
}

CommitDiffSource::~CommitDiffSource()
{
    cancel();
    for (auto *child : findChildren<QProcess *>(Qt::FindDirectChildrenOnly)) {
        child->disconnect(this);
        if (child->state() != QProcess::NotRunning) {
            child->kill();
            child->waitForFinished(100);
        }
    }
}

void CommitDiffSource::loadFiles(const QString &fullHash)
{
    cancel();
    if (!validObject(fullHash.toLatin1())) {
        emit failed(tr("The selected commit ID is invalid."));
        return;
    }
    run({QStringLiteral("show"), QStringLiteral("--format="), QStringLiteral("--root"),
         QStringLiteral("--raw"), QStringLiteral("-z"), QStringLiteral("--no-abbrev"),
         QStringLiteral("--diff-merges=first-parent"), QStringLiteral("--no-notes"),
         QStringLiteral("--no-show-signature"), QStringLiteral("--no-color"),
         QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"),
         QStringLiteral("--no-relative"), QStringLiteral("--find-renames"),
         QStringLiteral("--ignore-submodules=none"), fullHash, QStringLiteral("--")},
        ListLimit, [this](Result result) {
            if (result.oversized) {
                emit failed(tr("This commit has too many changed files to display."));
                return;
            }
            if (!result.error.isEmpty()) {
                emit failed(result.error);
                return;
            }
            QList<File> files;
            if (!parseFiles(result.data, files)) {
                emit failed(tr("Could not read the list of files returned by Git."));
                return;
            }
            emit filesReady(files);
        });
}

void CommitDiffSource::loadPreview(const File &file)
{
    cancel();
    if (file.oldMode == "160000" || file.newMode == "160000") {
        unavailable(tr("This change updates a Git submodule."));
        return;
    }
    if (!supportedMode(file.oldMode) || !supportedMode(file.newMode)) {
        unavailable(tr("This file type has no text preview."));
        return;
    }
    if (!validBlob(file.oldObject, file.oldMode) || !validBlob(file.newObject, file.newMode)) {
        emit failed(tr("Git did not provide a valid blob ID for this file."));
        return;
    }
    if (file.oldObject.isEmpty()) {
        loadAfter(file, {});
        return;
    }
    loadBlob(file.oldObject, [this, file](QString before) {
        Preview preview;
        preview.before = std::move(before);
        if (file.newObject == file.oldObject) {
            preview.after = preview.before;
            emit previewReady(preview);
        } else {
            loadAfter(file, std::move(preview));
        }
    });
}

void CommitDiffSource::loadWorkingFiles()
{
    run({QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")}, ListLimit, [this](Result result) {
        if (!result.error.isEmpty() || result.oversized) {
            emit failed(result.oversized ? tr("Could not determine the Git worktree directory.") : result.error);
            return;
        }
        if (result.data.endsWith('\n'))
            result.data.chop(1);
        const QString path = QString::fromUtf8(result.data);
        workingRoot_ = QFileInfo(path).canonicalFilePath();
        if (workingRoot_.isEmpty() || path.toUtf8() != result.data) {
            emit failed(tr("Could not open the Git worktree directory."));
            return;
        }
        run(GitStatus::arguments(), ListLimit, [this](Result status) {
            if (!status.error.isEmpty() || status.oversized) {
                emit failed(status.oversized ? tr("The worktree has too many changes to display.") : status.error);
                return;
            }
            emit filesReady(workingFiles(status.data));
        }, workingRoot_);
    });
}

void CommitDiffSource::loadWorkingPreview(const File &file)
{
    cancel();
    if (workingRoot_.isEmpty()) {
        emit failed(tr("Load the worktree changes before opening a file."));
        return;
    }

    // A deleted path may have been recreated after the status was loaded.
    const auto path = file.workingPath.isEmpty() ? file.newPath : file.workingPath;
    auto worktree = readWorktreeFile(workingRoot_, path);
    Preview preview;
    switch (worktree.state) {
    case WorktreeRead::Text:
        if (!decodeBlob(worktree.bytes, preview.after))
            return;
        break;
    case WorktreeRead::Missing:
        break;
    case WorktreeRead::Oversized:
        unavailable(tr("This file exceeds the 1 MiB preview limit."));
        return;
    case WorktreeRead::Unsupported:
        unavailable(tr("This file type has no text preview."));
        return;
    case WorktreeRead::Changed:
        emit failed(tr("The file changed during preview. Open it again to retry."));
        return;
    case WorktreeRead::Error:
        emit failed(tr("Could not read the file in the worktree. %1").arg(worktree.error));
        return;
    }
    loadWorkingBefore(file, std::move(preview), worktree.state == WorktreeRead::Text);
}

void CommitDiffSource::loadWorkingBefore(const File &file, Preview preview, bool afterExists)
{
    const auto oldPath = file.oldPath.isEmpty() ? file.workingPath : file.oldPath;
    if (oldPath.isEmpty()) {
        compareWorking(std::move(preview), false, afterExists);
        return;
    }
    const auto path = QString::fromUtf8(oldPath);
    if (path.toUtf8() != oldPath || oldPath.contains('\0')) {
        unavailable(tr("This file's path cannot be displayed by the Git preview."));
        return;
    }

    run({QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("--quiet"),
         QStringLiteral("HEAD^{commit}")}, ErrorLimit,
        [this, path, oldPath, preview = std::move(preview), afterExists](Result head) mutable {
            if (head.oversized || !head.error.isEmpty()) {
                emit failed(head.oversized ? tr("Could not read Git HEAD.") : head.error);
                return;
            }
            if (head.data.isEmpty()) {
                compareWorking(std::move(preview), false, afterExists);
                return;
            }
            const auto hash = head.data.trimmed();
            if (!validObject(hash)) {
                emit failed(tr("Git did not provide a valid HEAD commit ID."));
                return;
            }
            run({QStringLiteral("ls-tree"), QStringLiteral("-z"), QStringLiteral("--full-tree"),
                 QString::fromLatin1(hash), QStringLiteral("--"), QStringLiteral(":(top,literal)") + path},
                ListLimit, [this, oldPath, preview = std::move(preview), afterExists](Result tree) mutable {
                    if (tree.oversized || !tree.error.isEmpty()) {
                        emit failed(tree.oversized ? tr("Could not read the file from Git HEAD.") : tree.error);
                        return;
                    }
                    for (const auto &record : tree.data.split('\0')) {
                        const auto offset = record.indexOf('\t');
                        if (offset < 0 || record.sliced(offset + 1) != oldPath)
                            continue;
                        const auto metadata = record.first(offset).split(' ');
                        if (metadata.size() != 3 || !validObject(metadata[2])) {
                            emit failed(tr("Git did not provide a valid file entry for HEAD."));
                            return;
                        }
                        if (metadata[0] == "160000") {
                            unavailable(tr("This change updates a Git submodule."));
                            return;
                        }
                        if (!supportedMode(metadata[0]) || metadata[1] != "blob") {
                            unavailable(tr("This file type has no text preview."));
                            return;
                        }
                        loadBlob(metadata[2], [this, preview = std::move(preview), afterExists](QString before) mutable {
                            preview.before = std::move(before);
                            compareWorking(std::move(preview), true, afterExists);
                        });
                        return;
                    }
                    compareWorking(std::move(preview), false, afterExists);
                }, workingRoot_);
        }, workingRoot_, true);
}

void CommitDiffSource::compareWorking(Preview preview, bool beforeExists, bool afterExists)
{
    if (!beforeExists || !afterExists) {
        addWholeFileHunk(preview);
        emit previewReady(preview);
        return;
    }
    if (preview.before == preview.after) {
        emit previewReady(preview);
        return;
    }

    const auto snapshots = std::make_shared<QTemporaryDir>();
    if (!snapshots->isValid() || !writeSnapshot(snapshots->filePath(QStringLiteral("before")), preview.before) ||
        !writeSnapshot(snapshots->filePath(QStringLiteral("after")), preview.after)) {
        emit failed(tr("Could not create temporary file versions for the comparison."));
        return;
    }
    auto arguments = QStringList{QStringLiteral("diff"), QStringLiteral("--no-index")} + hunkOptions();
    arguments << QStringLiteral("--") << QStringLiteral("before") << QStringLiteral("after");
    run(std::move(arguments), PatchLimit, [this, snapshots, preview = std::move(preview)](Result result) mutable {
        finishComparison(std::move(preview), std::move(result));
    }, snapshots->path(), true);
}

void CommitDiffSource::loadAfter(const File &file, Preview preview)
{
    if (file.newObject.isEmpty()) {
        loadHunks(file, std::move(preview));
        return;
    }
    loadBlob(file.newObject, [this, file, preview = std::move(preview)](QString after) mutable {
        preview.after = std::move(after);
        loadHunks(file, std::move(preview));
    });
}

void CommitDiffSource::loadBlob(const QByteArray &object, std::function<void(QString)> completion)
{
    run({QStringLiteral("cat-file"), QStringLiteral("blob"), QString::fromLatin1(object)}, BlobLimit,
        [this, completion = std::move(completion)](Result result) {
            if (result.oversized) {
                unavailable(tr("This file exceeds the 1 MiB preview limit."));
                return;
            }
            if (!result.error.isEmpty()) {
                emit failed(result.error);
                return;
            }
            QString text;
            if (decodeBlob(result.data, text))
                completion(std::move(text));
        });
}

bool CommitDiffSource::decodeBlob(const QByteArray &data, QString &text)
{
    if (data.contains('\0')) {
        unavailable(tr("This binary file has no text preview."));
        return false;
    }
    QStringDecoder decode(QStringDecoder::Utf8,
        QStringDecoder::Flag::Stateless | QStringDecoder::Flag::ConvertInitialBom);
    text = decode(data);
    if (decode.hasError()) {
        unavailable(tr("This file is not valid UTF-8 text."));
        return false;
    }
    if (lineCount(text) > LineLimit) {
        unavailable(tr("This file exceeds the 50,000-line preview limit."));
        return false;
    }
    return true;
}

void CommitDiffSource::loadHunks(const File &file, Preview preview)
{
    if (file.oldObject.isEmpty() || file.newObject.isEmpty()) {
        addWholeFileHunk(preview);
        emit previewReady(preview);
        return;
    }

    auto arguments = QStringList{QStringLiteral("diff")} + hunkOptions();
    arguments << QString::fromLatin1(file.oldObject) << QString::fromLatin1(file.newObject) << QStringLiteral("--");
    run(std::move(arguments), PatchLimit, [this, preview = std::move(preview)](Result result) mutable {
            finishComparison(std::move(preview), std::move(result));
        });
}

void CommitDiffSource::finishComparison(Preview preview, Result result)
{
    if (result.oversized) {
        unavailable(tr("The changes in this file are too large to compare."));
        return;
    }
    if (!result.error.isEmpty()) {
        emit failed(result.error);
        return;
    }
    preview.hunks = parseHunks(result.data);
    if (preview.hunks.isEmpty() && preview.before != preview.after) {
        unavailable(tr("Git did not provide a text diff for this file."));
        return;
    }
    emit previewReady(preview);
}

void CommitDiffSource::unavailable(const QString &reason)
{
    Preview preview;
    preview.unavailableReason = reason;
    emit previewReady(preview);
}

void CommitDiffSource::run(QStringList arguments, qsizetype limit, Completion completion,
                           const QString &directory, bool acceptsExitOne)
{
    cancel();
    auto *process = new QProcess(this);
    process_ = process;
    process->setWorkingDirectory(directory.isEmpty() ? repository_ : directory);
    process->setStandardInputFile(QProcess::nullDevice());

    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto &variable : {QStringLiteral("GIT_DIR"), QStringLiteral("GIT_WORK_TREE"),
                                 QStringLiteral("GIT_INDEX_FILE"), QStringLiteral("GIT_EXTERNAL_DIFF"),
                                 QStringLiteral("GIT_DIFF_OPTS")})
        environment.remove(variable);
    environment.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_NO_LAZY_FETCH"), QStringLiteral("1"));
    process->setProcessEnvironment(environment);
    arguments.prepend(QStringLiteral("--no-pager"));

    struct Output {
        QByteArray data;
        QByteArray diagnostics;
        bool oversized = false;
    };
    const auto output = std::make_shared<Output>();
    const auto drain = [process, output, limit] {
        const auto available = process->readAllStandardOutput();
        if (!output->oversized) {
            if (available.size() > limit - output->data.size()) {
                output->oversized = true;
                if (process->state() != QProcess::NotRunning)
                    process->kill();
            } else {
                output->data += available;
            }
        }
        const auto diagnostics = process->readAllStandardError();
        const auto remaining = ErrorLimit - output->diagnostics.size();
        if (remaining > 0)
            output->diagnostics.append(diagnostics.constData(), std::min(diagnostics.size(), remaining));
    };
    connect(process, &QProcess::readyReadStandardOutput, this, drain);
    connect(process, &QProcess::readyReadStandardError, this, drain);

    const auto finish = [this, process, completion = std::move(completion)](Result result) {
        process->deleteLater();
        if (process_ != process)
            return;
        process_ = nullptr;
        completion(std::move(result));
    };
    connect(process, &QProcess::finished, this,
        [process, output, drain, finish, acceptsExitOne](int code, QProcess::ExitStatus status) {
            drain();
            QString error;
            const bool success = status == QProcess::NormalExit &&
                                 (code == 0 || (code == 1 && acceptsExitOne && output->diagnostics.isEmpty()));
            if (!output->oversized && !success) {
                error = QString::fromUtf8(output->diagnostics).trimmed();
                if (error.isEmpty())
                    error = process->errorString();
            }
            finish({std::move(output->data), std::move(error), output->oversized});
        });
    connect(process, &QProcess::errorOccurred, this, [process, finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish({{}, tr("Git could not start. %1").arg(process->errorString())});
    });
    process->start(QStringLiteral("git"), arguments, QIODevice::ReadOnly);
}

void CommitDiffSource::cancel()
{
    if (!process_)
        return;
    auto *process = std::exchange(process_, nullptr);
    if (process->state() != QProcess::NotRunning)
        process->kill();
}
