#pragma once

#include <QString>
#include <QVector>

#include <functional>

class QWidget;

namespace GitSquash {
struct Commit {
    QString hash;
    QString subject;
};

// Git log rows are passed newest first.
void request(QWidget *parent, const QString &root, const QVector<Commit> &commits,
             std::function<void()> completed);
}
