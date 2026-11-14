#pragma once

#include <QPlainTextEdit>

class SyntaxHighlighting;

class TextFileEditor : public QPlainTextEdit
{
    Q_OBJECT

public:
    enum class LoadResult { Ready, Preview, Unavailable };

    explicit TextFileEditor(QWidget *parent = nullptr);

    LoadResult openFile(const QString &absolutePath, qsizetype limit, const QString &expectedText = {});
    void clearFile(const QString &message = {});
    void reloadIfClean();
    bool confirmClose();
    bool save();
    QString editorText() const;
    void setSyntaxFileName(const QString &name);

signals:
    void saved();

private:
    QString path_;
    QString target_;
    QByteArray original_;
    QByteArray newline_ = "\n";
    qsizetype limit_ = 0;
    bool bom_ = false;
    SyntaxHighlighting *syntax_ = nullptr;
};
