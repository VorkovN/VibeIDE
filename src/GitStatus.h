#pragma once

#include <QByteArray>
#include <QList>
#include <QStringList>

namespace GitStatus {
struct Change {
    QByteArray path;
    QByteArray previousPath;
    QByteArray status;
};

inline QStringList arguments()
{
    return {QStringLiteral("status"), QStringLiteral("--porcelain=v1"), QStringLiteral("-z"),
            QStringLiteral("--untracked-files=all"), QStringLiteral("--ignore-submodules=none")};
}

inline QString displayPath(const QByteArray &path)
{
    QString text = QString::fromUtf8(path);
    text.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    text.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    text.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    return text;
}

inline QList<Change> parse(const QByteArray &status)
{
    const auto records = status.split('\0');
    QList<Change> changes;
    for (qsizetype i = 0; i < records.size(); ++i) {
        const auto &entry = records.at(i);
        if (entry.size() < 4 || entry.at(2) != ' ' || entry.startsWith("!!"))
            continue;
        const char index = entry.at(0);
        const char worktree = entry.at(1);
        QByteArray previousPath;
        if (index == 'R' || index == 'C' || worktree == 'R' || worktree == 'C') {
            if (++i >= records.size())
                break;
            previousPath = records.at(i);
        }
        changes.append({entry.mid(3), previousPath, entry.left(2)});
    }
    return changes;
}
}
