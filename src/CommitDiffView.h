#pragma once

class QByteArray;
class QString;
class QWidget;
class TextFileEditor;

namespace CommitDiffView {
QWidget *create(QWidget *parent, const QString &repository, const QString &hash,
                const QString &subject);
QWidget *createWorking(QWidget *parent, const QString &repository, const QByteArray &path);
TextFileEditor *workingEditor(QWidget *page);
bool confirmClose(QWidget *page);
}
