#pragma once

#include <QStringList>
#include <Qt>

namespace FileTransfer {
struct Result {
    QStringList errors;
};

QStringList topLevelPaths(const QStringList &paths);
Result transfer(const QStringList &sources, const QString &destinationFolder, Qt::DropAction action);
}
