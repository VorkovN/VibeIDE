#pragma once

#include <QPlainTextEdit>
#include <QTimer>

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
    bool flushAutosave();
    const QString &autosaveError() const { return autosaveError_; }
    QString editorText() const;
    void setSyntaxFileName(const QString &name);
    void setFindText(const QString &text);
    qsizetype findMatchCount() const { return findMatches_.size(); }
    void stepFindMatch(bool backwards);

signals:
    void saved();
    void autosaveErrorChanged(const QString &message);
    void findMatchesChanged();

protected:
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void setBaseSelections(QList<QTextEdit::ExtraSelection> selections);

private:
    void setAutosaveError(const QString &message);
    void rebuildFindMatches();
    void updateSelections();
    QTextCursor findCursor(int position) const;

    QString path_;
    QString target_;
    QString autosaveError_;
    QTimer autosave_;
    QByteArray original_;
    QByteArray newline_ = "\n";
    qsizetype limit_ = 0;
    bool bom_ = false;
    SyntaxHighlighting *syntax_ = nullptr;
    QString findText_;
    QList<int> findMatches_;
    QList<QTextEdit::ExtraSelection> baseSelections_;
};
