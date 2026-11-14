#pragma once

class QByteArray;
class QString;
class QWidget;

namespace CommitDiffView {
QWidget *create(QWidget *parent, const QString &repository, const QString &hash,
                const QString &subject);
QWidget *createWorking(QWidget *parent, const QString &repository, const QByteArray &path);
}
