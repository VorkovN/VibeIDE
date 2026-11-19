#pragma once

#include <QProcessEnvironment>
#include <QString>

namespace PythonLaunch {
QString prepare(const QString &program, const QString &applicationName, const QString &stableId,
                QProcessEnvironment &environment, QString &error);
}
