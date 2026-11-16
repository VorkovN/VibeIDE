#include "TextFileEditor.h"

#include "SyntaxHighlighting.h"

#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
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
    autosave_.setSingleShot(true);
    autosave_.setInterval(1000);
    autosave_.setTimerType(Qt::PreciseTimer);
    connect(&autosave_, &QTimer::timeout, this, &TextFileEditor::flushAutosave);
    connect(document(), &QTextDocument::contentsChanged, this, [this] {
        if (!isReadOnly() && !autosave_.isActive())
            autosave_.start();
    });
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
    document()->setModified(false);
    setReadOnly(false);
    return LoadResult::Ready;
}

void TextFileEditor::clearFile(const QString &message)
{
    autosave_.stop();
    path_.clear();
    target_.clear();
    original_.clear();
    setReadOnly(true);
    setSyntaxFileName({});
    clear();
    document()->setModified(false);
    setPlaceholderText(message);
    setAutosaveError({});
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

void TextFileEditor::setAutosaveError(const QString &message)
{
    if (autosaveError_ == message)
        return;
    autosaveError_ = message;
    emit autosaveErrorChanged(autosaveError_);
}

bool TextFileEditor::flushAutosave()
{
    autosave_.stop();
    if (isReadOnly() || !document()->isModified()) {
        setAutosaveError({});
        return true;
    }

    QByteArray bytes = editorText().toUtf8();
    if (newline_ != "\n")
        bytes.replace("\n", newline_);
    if (bom_)
        bytes.prepend(utf8Bom);

    if (bytes == original_) {
        document()->setModified(false);
        setAutosaveError({});
        return true;
    }

    const auto unchangedOnDisk = [this] {
        const QFileInfo info(path_);
        QByteArray current;
        return !info.isSymLink() && info.canonicalFilePath() == target_
            && readFile(target_, original_.size(), current) && current == original_;
    };
    const auto diskConflict = [this] {
        setAutosaveError(tr("Could not autosave %1 because it changed or was removed on disk. Your edits are still open.")
            .arg(QFileInfo(path_).fileName()));
        return false;
    };
    if (!unchangedOnDisk())
        return diskConflict();

    QSaveFile file(target_);
    const auto writeFailed = [this, &file] {
        setAutosaveError(tr("Could not autosave %1. %2").arg(QFileInfo(path_).fileName(), file.errorString()));
        return false;
    };
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        return writeFailed();
    if (!unchangedOnDisk())
        return diskConflict();
    if (!file.commit())
        return writeFailed();

    original_ = std::move(bytes);
    document()->setModified(false);
    setAutosaveError({});
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
