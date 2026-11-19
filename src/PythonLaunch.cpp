#include "PythonLaunch.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QXmlStreamWriter>

#include <utility>

namespace PythonLaunch {
QString prepare(const QString &program, const QString &applicationName, const QString &stableId,
                QProcessEnvironment &environment, QString &error)
{
    error.clear();
    const auto interpreter = QFileInfo(program).isAbsolute() ? program
        : QStandardPaths::findExecutable(program,
              environment.value(QStringLiteral("PATH")).split(QLatin1Char(':'), Qt::SkipEmptyParts));
    if (interpreter.isEmpty() || !QFileInfo(interpreter).isExecutable()) {
        error = QStringLiteral("The Python interpreter “%1” is unavailable.").arg(program);
        return {};
    }

    // Framework Python first execs its embedded Python.app. Link the final image,
    // rather than that stub, so macOS keeps this configuration's bundle identity.
    QProcess probe;
    probe.setProcessEnvironment(environment);
    probe.start(interpreter, {QStringLiteral("-I"), QStringLiteral("-S"), QStringLiteral("-c"),
        QStringLiteral(
            "import ctypes,json,os\n"
            "f=ctypes.CDLL(None)._NSGetExecutablePath\n"
            "f.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_uint32)]\n"
            "b=ctypes.create_string_buffer(32768); n=ctypes.c_uint32(len(b))\n"
            "if f(b,ctypes.byref(n)): raise RuntimeError('Executable path unavailable')\n"
            "print(json.dumps([os.path.realpath(os.fsdecode(b.value))]))\n")});
    if (!probe.waitForStarted(1000) || !probe.waitForFinished(2000)) {
        probe.kill();
        probe.waitForFinished(300);
        error = QStringLiteral("The Python interpreter did not respond while preparing its application name.");
        return {};
    }
    const auto paths = QJsonDocument::fromJson(probe.readAllStandardOutput()).array();
    const auto image = paths.isEmpty() ? QString() : paths.first().toString();
    if (probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0 ||
        image.isEmpty() || !QFileInfo(image).isExecutable()) {
        error = QStringLiteral("Could not locate the Python executable for “%1”.\n%2")
                    .arg(program, QString::fromUtf8(probe.readAllStandardError()).trimmed());
        return {};
    }

    auto fileName = applicationName;
    fileName.replace(QRegularExpression(QStringLiteral("[\\x00-\\x1f/:]")), QStringLiteral("_"));
    while (fileName.toUtf8().size() > 160)
        fileName.chop(fileName.back().isLowSurrogate() ? 2 : 1);
    if (fileName.isEmpty() || fileName == QStringLiteral(".") || fileName == QStringLiteral(".."))
        fileName = QStringLiteral("Python Application");
    const auto identity = stableId.toUtf8() + '\0' + applicationName.toUtf8() + '\0' +
                          interpreter.toUtf8() + '\0' + image.toUtf8();
    const auto hash = QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
    const auto contents = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                          QStringLiteral("/python-launchers/") + hash + QLatin1Char('/') +
                          fileName + QStringLiteral(".app/Contents");
    const auto executable = contents + QStringLiteral("/MacOS/") + fileName;
    if (!QDir().mkpath(contents + QStringLiteral("/MacOS"))) {
        error = QStringLiteral("Could not create the named Python application.");
        return {};
    }
    if (QFileInfo(executable).symLinkTarget() != image) {
        if (QFileInfo::exists(executable) || QFileInfo(executable).isSymLink())
            QFile::remove(executable);
        if (!QFile::link(image, executable)) {
            error = QStringLiteral("Could not link the Python interpreter into its named application.");
            return {};
        }
    }
    const auto plistPath = contents + QStringLiteral("/Info.plist");
    if (!QFileInfo::exists(plistPath)) {
        QSaveFile file(plistPath);
        if (!file.open(QIODevice::WriteOnly)) {
            error = file.errorString();
            return {};
        }
        QXmlStreamWriter xml(&file);
        xml.writeStartDocument();
        xml.writeDTD(QStringLiteral("<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"));
        xml.writeStartElement(QStringLiteral("plist"));
        xml.writeAttribute(QStringLiteral("version"), QStringLiteral("1.0"));
        xml.writeStartElement(QStringLiteral("dict"));
        for (const auto &[key, value] : {
                 std::pair{QStringLiteral("CFBundleName"), applicationName},
                 std::pair{QStringLiteral("CFBundleDisplayName"), applicationName},
                 std::pair{QStringLiteral("CFBundleExecutable"), fileName},
                 std::pair{QStringLiteral("CFBundleIdentifier"), QStringLiteral("app.vibeide.python.") + hash},
                 std::pair{QStringLiteral("CFBundlePackageType"), QStringLiteral("APPL")},
                 std::pair{QStringLiteral("NSCameraUsageDescription"),
                           QStringLiteral("Projects you run from VibeIDE may request access to your camera to use their camera features.")}}) {
            xml.writeTextElement(QStringLiteral("key"), key);
            xml.writeTextElement(QStringLiteral("string"), value);
        }
        xml.writeEndElement();
        xml.writeEndElement();
        xml.writeEndDocument();
        if (!file.commit()) {
            error = file.errorString();
            return {};
        }
    }
    // CPython consumes this before user code, preserving the selected venv and
    // sys.executable without leaking a different interpreter into child processes.
    if (!environment.contains(QStringLiteral("__PYVENV_LAUNCHER__")))
        environment.insert(QStringLiteral("__PYVENV_LAUNCHER__"), interpreter);
    return executable;
}
}
