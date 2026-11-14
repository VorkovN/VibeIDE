#pragma once

#include <QSyntaxHighlighter>

class QPlainTextEdit;

namespace Syntax {
enum class Language;
}

class SyntaxHighlighting final : public QSyntaxHighlighter
{
public:
    explicit SyntaxHighlighting(QPlainTextEdit *editor);
    void setFileName(const QString &path);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void highlightBlock(const QString &text) override;

private:
    const QPlainTextEdit *editor_;
    Syntax::Language language_;
    bool dark_;
};
