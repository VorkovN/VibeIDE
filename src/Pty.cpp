#include "Pty.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSocketNotifier>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <vector>

#include <fcntl.h>
#include <libproc.h>
#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <util.h>

namespace {

winsize terminalSize(int rows, int cols)
{
    constexpr int maximum = std::numeric_limits<unsigned short>::max();
    winsize size{};
    size.ws_row = static_cast<unsigned short>(std::clamp(rows, 1, maximum));
    size.ws_col = static_cast<unsigned short>(std::clamp(cols, 1, maximum));
    return size;
}

QString shellDirectory(pid_t pid)
{
    proc_vnodepathinfo info{};
    if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &info, sizeof(info)) != sizeof(info))
        return {};
    return QFile::decodeName(info.pvi_cdir.vip_path);
}

QProcessEnvironment terminalEnvironment()
{
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    environment.remove(QStringLiteral("LINES"));
    environment.remove(QStringLiteral("COLUMNS"));

    auto locale = environment.value(QStringLiteral("LC_ALL"));
    if (locale.isEmpty())
        locale = environment.value(QStringLiteral("LC_CTYPE"));
    if (locale.isEmpty())
        locale = environment.value(QStringLiteral("LANG"));
    if (!locale.contains(QStringLiteral("utf-8"), Qt::CaseInsensitive)
        && !locale.contains(QStringLiteral("utf8"), Qt::CaseInsensitive)) {
        environment.remove(QStringLiteral("LC_ALL"));
        environment.insert(QStringLiteral("LC_CTYPE"), QStringLiteral("en_US.UTF-8"));
    }
    return environment;
}

QByteArray shellProgram(const QProcessEnvironment &environment)
{
    auto path = environment.value(QStringLiteral("SHELL"));
    if (path.isEmpty()) {
        if (const passwd *user = ::getpwuid(::getuid()))
            path = QFile::decodeName(user->pw_shell);
    }
    const QFileInfo shell(path);
    if (shell.isAbsolute() && shell.isFile() && shell.isExecutable())
        return QFile::encodeName(shell.absoluteFilePath());
    return "/bin/sh";
}

bool closeOnExec(int descriptor)
{
    return ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0;
}

[[noreturn]] void reportStartupFailure(int descriptor)
{
    const int startupError = errno;
    while (::write(descriptor, &startupError, sizeof(startupError)) < 0 && errno == EINTR) {
    }
    ::_exit(127);
}

void terminateChild(pid_t pid)
{
    if (pid <= 0)
        return;

    int status = 0;
    pid_t result;
    do {
        result = ::waitpid(pid, &status, WNOHANG);
    } while (result == -1 && errno == EINTR);

    if (result == 0) {
        ::kill(pid, SIGKILL);
        while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {
        }
    }
}

} // namespace

Pty::Pty(QObject *parent)
    : QObject(parent)
{
    childTimer_.setInterval(500);
    connect(&childTimer_, &QTimer::timeout, this, &Pty::checkChild);
}

Pty::~Pty()
{
    closeMaster();
    terminateChild(child_);
}

bool Pty::start(const QString &directory, int rows, int cols)
{
    if (child_ > 0) {
        error_ = tr("The terminal is already running.");
        return false;
    }
    error_.clear();
    if (!QFileInfo(directory).isDir()) {
        error_ = tr("Terminal directory does not exist: %1").arg(directory);
        return false;
    }

    const QString absoluteDirectory = QDir(directory).absolutePath();
    const QByteArray nativeDirectory = QFile::encodeName(absoluteDirectory);
    const auto environment = terminalEnvironment();
    const QByteArray shell = shellProgram(environment);

    std::vector<QByteArray> environmentStorage;
    const auto environmentEntries = environment.toStringList();
    environmentStorage.reserve(environmentEntries.size());
    for (const QString &entry : environmentEntries)
        environmentStorage.push_back(entry.toLocal8Bit());

    std::vector<char *> environmentPointers;
    environmentPointers.reserve(environmentStorage.size() + 1);
    for (QByteArray &entry : environmentStorage)
        environmentPointers.push_back(entry.data());
    environmentPointers.push_back(nullptr);

    QByteArray shellName = shell.mid(shell.lastIndexOf('/') + 1);
    // Login shells load the user's PATH when the application starts from Finder.
    shellName.prepend('-');
    char *arguments[] = {shellName.data(), nullptr};
    const char *program = shell.constData();
    const char *startDirectory = nativeDirectory.constData();
    char *const *environmentData = environmentPointers.data();
    auto size = terminalSize(rows, cols);

    int startupPipe[2];
    if (::pipe(startupPipe) == -1) {
        error_ = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    if (!closeOnExec(startupPipe[0]) || !closeOnExec(startupPipe[1])) {
        error_ = QString::fromLocal8Bit(std::strerror(errno));
        ::close(startupPipe[0]);
        ::close(startupPipe[1]);
        return false;
    }

    int master;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &size);
    if (pid == 0) {
        ::close(startupPipe[0]);
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        const int signalsToRestore[] = {
            SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGPIPE, SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD
        };
        for (const int signal : signalsToRestore)
            ::sigaction(signal, &action, nullptr);
        ::sigprocmask(SIG_SETMASK, &action.sa_mask, nullptr);

        if (::chdir(startDirectory) == -1)
            reportStartupFailure(startupPipe[1]);
        ::execve(program, arguments, environmentData);

        char fallback[] = "/bin/sh";
        char fallbackName[] = "-sh";
        arguments[0] = fallbackName;
        ::execve(fallback, arguments, environmentData);
        reportStartupFailure(startupPipe[1]);
    }
    const int forkError = errno;
    ::close(startupPipe[1]);
    if (pid == -1) {
        ::close(startupPipe[0]);
        error_ = QString::fromLocal8Bit(std::strerror(forkError));
        return false;
    }

    int startupError = 0;
    ssize_t startupResult;
    do {
        startupResult = ::read(startupPipe[0], &startupError, sizeof(startupError));
    } while (startupResult == -1 && errno == EINTR);
    if (startupResult == -1)
        startupError = errno;
    ::close(startupPipe[0]);
    if (startupResult != 0) {
        error_ = tr("Could not start a shell in %1 (%2).")
                     .arg(absoluteDirectory, QString::fromLocal8Bit(std::strerror(startupError)));
        ::close(master);
        terminateChild(pid);
        return false;
    }

    const int flags = ::fcntl(master, F_GETFL);
    if (flags == -1 || ::fcntl(master, F_SETFL, flags | O_NONBLOCK) == -1
        || !closeOnExec(master)) {
        error_ = QString::fromLocal8Bit(std::strerror(errno));
        ::close(master);
        terminateChild(pid);
        return false;
    }

    master_ = master;
    child_ = pid;
    directory_ = absoluteDirectory;
    readNotifier_ = new QSocketNotifier(master_, QSocketNotifier::Read, this);
    writeNotifier_ = new QSocketNotifier(master_, QSocketNotifier::Write, this);
    writeNotifier_->setEnabled(false);
    connect(readNotifier_, &QSocketNotifier::activated, this, &Pty::readAvailable);
    connect(writeNotifier_, &QSocketNotifier::activated, this, &Pty::flushWrites);
    childTimer_.start();
    return true;
}

void Pty::write(const QByteArray &data)
{
    if (master_ < 0 || data.isEmpty())
        return;
    pendingWrites_.append(data);
    flushWrites();
}

void Pty::resize(int rows, int cols)
{
    if (master_ < 0)
        return;
    auto size = terminalSize(rows, cols);
    ::ioctl(master_, TIOCSWINSZ, &size);
}

QString Pty::workingDirectory() const
{
    if (child_ > 0) {
        const QString currentDirectory = shellDirectory(child_);
        if (!currentDirectory.isEmpty())
            directory_ = currentDirectory;
    }
    return directory_;
}

QString Pty::errorString() const
{
    return error_;
}

bool Pty::isRunning() const
{
    return child_ > 0;
}

void Pty::readAvailable()
{
    QByteArray data;
    char buffer[8192];
    while (master_ >= 0 && data.size() < 128 * 1024) {
        const ssize_t length = ::read(master_, buffer, sizeof(buffer));
        if (length > 0) {
            data.append(buffer, length);
            continue;
        }
        if (length < 0 && errno == EINTR)
            continue;
        if (length == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            readNotifier_->setEnabled(false);
        break;
    }
    if (!data.isEmpty())
        emit dataReceived(data);
}

void Pty::flushWrites()
{
    if (master_ < 0)
        return;

    while (!pendingWrites_.isEmpty()) {
        const ssize_t length = ::write(master_, pendingWrites_.constData(), pendingWrites_.size());
        if (length > 0) {
            pendingWrites_.remove(0, length);
            continue;
        }
        if (length < 0 && errno == EINTR)
            continue;
        if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            pendingWrites_.clear();
        break;
    }
    writeNotifier_->setEnabled(!pendingWrites_.isEmpty());
}

void Pty::checkChild()
{
    workingDirectory();
    int status = 0;
    const pid_t result = ::waitpid(child_, &status, WNOHANG);
    if (result == 0 || (result == -1 && errno == EINTR))
        return;

    childTimer_.stop();
    child_ = -1;
    readAvailable();
    closeMaster();
    emit finished();
}

void Pty::closeMaster()
{
    if (master_ < 0)
        return;
    readNotifier_->setEnabled(false);
    writeNotifier_->setEnabled(false);
    readNotifier_->deleteLater();
    writeNotifier_->deleteLater();
    readNotifier_ = nullptr;
    writeNotifier_ = nullptr;
    ::close(master_);
    master_ = -1;
    pendingWrites_.clear();
}
