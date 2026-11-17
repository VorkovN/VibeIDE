#include "FileTransfer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <cerrno>
#include <copyfile.h>
#include <stdio.h>
#include <sys/stat.h>
#include <system_error>

namespace {
QString tr(const char *text)
{
    return QCoreApplication::translate("FileTransfer", text);
}

QString cleanSourcePath(const QString &path)
{
    if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null))
        return path;
    const QFileInfo info(QDir::cleanPath(path));
    // Resolve aliases in the parent and leave the selected entry itself intact.
    const auto parent = info.absoluteDir().canonicalPath();
    return parent.isEmpty() || info.fileName().isEmpty() ? info.absoluteFilePath()
        : QDir(parent).filePath(info.fileName());
}

bool sameEntry(const struct stat &first, const struct stat &second)
{
    return first.st_dev == second.st_dev && first.st_ino == second.st_ino;
}

QString systemError(int error)
{
    return QString::fromLocal8Bit(std::error_code(error, std::generic_category()).message().c_str());
}

QString transferError(int error, const QString &destination, Qt::DropAction action)
{
    if (error == EEXIST || error == ENOTEMPTY)
        return tr("An item named “%1” already exists in the destination.").arg(QFileInfo(destination).fileName());
    if (error == EXDEV && action == Qt::MoveAction)
        return tr("Moving items across volumes is unavailable. Copy the item instead.");
    return systemError(error);
}

QString duplicateName(const QFileInfo &source, bool directory, int number)
{
    const auto name = source.fileName();
    const auto dot = directory ? -1 : name.lastIndexOf(u'.');
    const auto extensionStart = dot > 0 ? dot : name.size();
    const auto suffix = number == 1 ? tr(" copy") : tr(" copy %1").arg(number);
    return name.left(extensionStart) + suffix + name.mid(extensionStart);
}

bool existsNoFollow(const QString &path)
{
    struct stat entry{};
    return lstat(QFile::encodeName(path).constData(), &entry) == 0;
}

int stopOnCopyError(int, int stage, copyfile_state_t, const char *, const char *, void *)
{
    return stage == COPYFILE_ERR ? COPYFILE_QUIT : COPYFILE_CONTINUE;
}

int copyToStage(const QString &source, const QString &staged, bool directory)
{
    auto state = copyfile_state_alloc();
    if (!state)
        return errno ? errno : ENOMEM;
    int result = copyfile_state_set(state, COPYFILE_STATE_STATUS_CB,
                                    reinterpret_cast<const void *>(&stopOnCopyError));
    if (result == 0) {
        const copyfile_flags_t flags = COPYFILE_ALL | COPYFILE_NOFOLLOW | COPYFILE_EXCL;
        result = copyfile(QFile::encodeName(source).constData(), QFile::encodeName(staged).constData(),
                          state, directory ? flags | COPYFILE_RECURSIVE : flags);
    }
    const int error = result == 0 ? 0 : (errno ? errno : EIO);
    copyfile_state_free(state);
    return error;
}

void makeStageRemovable(const QString &path)
{
    const auto encoded = QFile::encodeName(path);
    struct stat entry{};
    if (lstat(encoded.constData(), &entry) != 0)
        return;

    // Only reset metadata on the private copy. Never follow a copied link.
    constexpr unsigned int protectedFlags = UF_IMMUTABLE | UF_APPEND | SF_IMMUTABLE | SF_APPEND;
    if (entry.st_flags & protectedFlags)
        lchflags(encoded.constData(), entry.st_flags & ~protectedFlags);
    if (!S_ISDIR(entry.st_mode))
        return;
    chmod(encoded.constData(), S_IRWXU);
    const QDir directory(path);
    const auto children = directory.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const auto &child : children)
        makeStageRemovable(directory.filePath(child));
}

QString copyItem(const QFileInfo &source, const QDir &destination, bool directory, bool duplicate)
{
    auto target = destination.filePath(source.fileName());
    // Exclusive rename still protects the target if another application creates it during the copy.
    if (!duplicate && existsNoFollow(target))
        return transferError(EEXIST, target, Qt::CopyAction);

    QTemporaryDir stage(destination.filePath(QStringLiteral(".vibeide-copy-XXXXXX")));
    if (!stage.isValid())
        return stage.errorString();
    const auto staged = stage.filePath(QStringLiteral("item"));
    const auto cleanupFailure = [&](const QString &error) {
        makeStageRemovable(staged);
        if (stage.remove())
            return error;
        stage.setAutoRemove(false);
        return tr("%1 A temporary copy remains in %2.").arg(error, stage.path());
    };
    if (const int error = copyToStage(source.absoluteFilePath(), staged, directory))
        return cleanupFailure(systemError(error));

    const auto stagedBytes = QFile::encodeName(staged);
    int copyNumber = 1;
    for (;;) {
        if (duplicate)
            target = destination.filePath(duplicateName(source, directory, copyNumber++));
        if (renamex_np(stagedBytes.constData(), QFile::encodeName(target).constData(), RENAME_EXCL) == 0)
            return {};
        const int error = errno;
        if (!duplicate || (error != EEXIST && error != ENOTEMPTY))
            return cleanupFailure(transferError(error, target, Qt::CopyAction));
    }
}

QString transferItem(const QString &path, const QDir &destination, const QList<struct stat> &ancestors,
                     Qt::DropAction action)
{
    if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null))
        return tr("The source path is invalid.");

    const QFileInfo source(path);
    if (source.fileName().isEmpty())
        return tr("Choose an item inside a volume.");

    const auto sourceBytes = QFile::encodeName(path);
    struct stat item{};
    if (lstat(sourceBytes.constData(), &item) != 0)
        return systemError(errno);
    const bool directory = S_ISDIR(item.st_mode);
    if (!directory && !S_ISREG(item.st_mode) && !S_ISLNK(item.st_mode))
        return tr("Only files, folders and symbolic links can be transferred.");
    if (directory && std::ranges::any_of(ancestors, [&](const auto &dir) { return sameEntry(item, dir); }))
        return tr("A folder cannot be transferred into itself or one of its subfolders.");

    struct stat parent{};
    if (stat(QFile::encodeName(source.absolutePath()).constData(), &parent) != 0)
        return systemError(errno);
    const bool sameFolder = sameEntry(parent, ancestors.first());
    if (action == Qt::CopyAction)
        return copyItem(source, destination, directory, sameFolder);
    if (sameFolder)
        return {};

    const auto target = destination.filePath(source.fileName());
    if (renamex_np(sourceBytes.constData(), QFile::encodeName(target).constData(), RENAME_EXCL) == 0)
        return {};
    return transferError(errno, target, action);
}
}

QStringList FileTransfer::topLevelPaths(const QStringList &paths)
{
    QStringList unique;
    for (const auto &path : paths) {
        const auto normalized = cleanSourcePath(path);
        if (!unique.contains(normalized))
            unique.append(normalized);
    }

    QStringList directoryPrefixes;
    for (const auto &path : unique) {
        const QFileInfo info(path);
        if (!path.contains(QChar::Null) && info.isAbsolute() && !info.isSymLink() && info.isDir())
            directoryPrefixes.append(path.endsWith(u'/') ? path : path + u'/');
    }

    QStringList selection;
    for (const auto &path : unique) {
        if (!std::ranges::any_of(directoryPrefixes, [&](const auto &prefix) {
                return path.size() > prefix.size() && path.startsWith(prefix);
            })) {
            selection.append(path);
        }
    }
    return selection;
}

FileTransfer::Result FileTransfer::transfer(const QStringList &sources, const QString &destinationFolder,
                                            Qt::DropAction action)
{
    if (action != Qt::CopyAction && action != Qt::MoveAction)
        return {{tr("Choose either Copy or Move.")}};

    const QFileInfo destinationInfo(destinationFolder);
    if (destinationFolder.contains(QChar::Null) || !destinationInfo.isAbsolute() || !destinationInfo.isDir())
        return {{tr("The destination folder is unavailable.")}};

    const auto canonical = destinationInfo.canonicalFilePath();
    if (canonical.isEmpty())
        return {{tr("The destination folder is unavailable.")}};
    const QDir destination(canonical);
    QList<struct stat> ancestors;
    auto folder = destination;
    do {
        struct stat entry{};
        if (stat(QFile::encodeName(folder.absolutePath()).constData(), &entry) != 0 || !S_ISDIR(entry.st_mode))
            return {{tr("The destination folder is unavailable.")}};
        ancestors.append(entry);
    } while (folder.cdUp());

    Result result;
    for (const auto &source : topLevelPaths(sources)) {
        const auto reason = transferItem(source, destination, ancestors, action);
        if (!reason.isEmpty())
            result.errors.append(tr("%1: %2").arg(source, reason));
    }
    return result;
}
