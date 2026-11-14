#include "TextFileEditor.h"

#include "SyntaxHighlighting.h"

#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QMessageBox>
#include <QSaveFile>
#include <QScrollBar>
#include <QStringDecoder>
#include <QTextDocument>

namespace {
constexpr auto utf8Bom = "\xef\xbb\xbf";

bool readFile(const QString &path, qsizetype limit, QByteArray &bytes)
{
    if (!QFileInfo(path).isFile())
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    bytes = file.read(limit + 1);
    return file.error() == QFileDevice::NoError;
}

QString normalizeNewlines(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    return text.replace(u'\r', u'\n');
}
}

TextFileEditor::TextFileEditor(QWidget *parent) : QPlainTextEdit(parent)
{
    setReadOnly(true);
    setLineWrapMode(NoWrap);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
}

TextFileEditor::LoadResult TextFileEditor::openFile(const QString &absolutePath, qsizetype limit,
                                                      const QString &expectedText)
{
    clearFile();
    const QFileInfo info(absolutePath);
    path_ = absolutePath;
    target_ = info.canonicalFilePath();
    limit_ = limit;
    QByteArray bytes;
    if (target_.isEmpty() || !readFile(target_, limit, bytes)) {
        setPlaceholderText(tr("Could not read this file."));
        return LoadResult::Unavailable;
    }

    const bool truncated = bytes.size() > limit;
    const bool hasBom = bytes.startsWith(utf8Bom);
    auto decoder = QStringDecoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const auto sample = QByteArrayView(bytes).first(qMin(bytes.size(), limit));
    // Keep a partial UTF-8 sequence at the preview boundary out of the decoder.
    qsizetype size = sample.size();
    if (truncated) {
        while (size > 0 && (static_cast<unsigned char>(bytes.at(size)) & 0xc0) == 0x80)
            --size;
    }
    const auto decoded = QString(decoder(sample.first(size)));
    if (bytes.contains('\0') || decoder.hasError() || (!truncated && decoded.toUtf8() !=
        (hasBom ? bytes.mid(3) : bytes))) {
        setPlaceholderText(tr("Preview is available for UTF-8 text files."));
        return LoadResult::Unavailable;
    }

    const auto text = normalizeNewlines(decoded);
    auto expected = normalizeNewlines(expectedText);
    if (hasBom && expected.startsWith(QChar::ByteOrderMark))
        expected.removeFirst();
    if (!expectedText.isNull() && text != expected) {
        setPlaceholderText(tr("The file changed while the comparison was loading. Reopen Changes to update it."));
        return LoadResult::Unavailable;
    }

    setPlaceholderText(tr("Empty file"));
    setPlainText(text);
    setSyntaxFileName(path_);

    const qsizetype crlf = bytes.count("\r\n");
    const qsizetype cr = bytes.count('\r');
    const qsizetype lf = bytes.count('\n');
    const bool mixedNewlines = (crlf > 0) + (cr > crlf) + (lf > crlf) > 1;
    if (truncated || info.isSymLink() || !info.isWritable() || mixedNewlines || editorText() != text)
        return LoadResult::Preview;

    newline_ = crlf > 0 ? QByteArray("\r\n") : cr > 0 ? QByteArray("\r") : QByteArray("\n");
    bom_ = hasBom;
    original_ = std::move(bytes);
    setReadOnly(false);
    return LoadResult::Ready;
}

void TextFileEditor::clearFile(const QString &message)
{
    path_.clear();
    target_.clear();
    original_.clear();
    setReadOnly(true);
    setSyntaxFileName({});
    clear();
    document()->setModified(false);
    setPlaceholderText(message);
}

void TextFileEditor::reloadIfClean()
{
    if (path_.isEmpty() || document()->isModified())
        return;
    if (!isReadOnly()) {
        QByteArray current;
        if (QFileInfo(path_).canonicalFilePath() == target_ &&
            readFile(target_, original_.size(), current) && current == original_)
            return;
    }

    const auto path = path_;
    const int position = textCursor().position();
    const int vertical = verticalScrollBar()->value();
    const int horizontal = horizontalScrollBar()->value();
    openFile(path, limit_);
    auto cursor = textCursor();
    cursor.setPosition(qMin(position, document()->characterCount() - 1));
    setTextCursor(cursor);
    verticalScrollBar()->setValue(vertical);
    horizontalScrollBar()->setValue(horizontal);
}

bool TextFileEditor::confirmClose()
{
    if (!document()->isModified())
        return true;
    const auto action = QMessageBox::warning(this, tr("Unsaved Changes"),
        tr("Save changes to %1?").arg(QFileInfo(path_).fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    return action == QMessageBox::Discard || (action == QMessageBox::Save && save());
}

bool TextFileEditor::save()
{
    if (!document()->isModified())
        return true;
    if (isReadOnly())
        return false;
    const QFileInfo info(path_);
    QByteArray current;
    if (info.isSymLink() || info.canonicalFilePath() != target_ ||
        !readFile(target_, original_.size(), current) || current != original_) {
        QMessageBox::warning(this, tr("File Changed on Disk"),
            tr("%1 was changed or removed outside this editor. Your edits are still open.").arg(path_));
        return false;
    }

    QByteArray bytes = editorText().toUtf8();
    if (newline_ != "\n")
        bytes.replace("\n", newline_);
    if (bom_)
        bytes.prepend(utf8Bom);

    QSaveFile file(target_);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        QMessageBox::warning(this, tr("Could Not Save File"), tr("%1\n%2").arg(path_, file.errorString()));
        return false;
    }
    original_ = std::move(bytes);
    document()->setModified(false);
    emit saved();
    return true;
}

QString TextFileEditor::editorText() const
{
    return document()->toRawText().replace(QChar::ParagraphSeparator, u'\n');
}

void TextFileEditor::setSyntaxFileName(const QString &name)
{
    if (!syntax_ && !name.isEmpty())
        syntax_ = new SyntaxHighlighting(this);
    if (syntax_)
        syntax_->setFileName(name);
}
