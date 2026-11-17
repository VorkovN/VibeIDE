#include "TextFileEditor.h"

#include "SyntaxHighlighting.h"

#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QSaveFile>
#include <QScrollBar>
#include <QStringDecoder>
#include <QStringMatcher>
#include <QTextDocument>

#include <algorithm>
#include <utility>

namespace {
constexpr auto utf8Bom = "\xef\xbb\xbf";
constexpr qsizetype maxFullFindHighlights = 1024;

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
        if (!findText_.isEmpty())
            rebuildFindMatches();
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (findMatches_.size() > maxFullFindHighlights)
            updateSelections();
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

void TextFileEditor::setFindText(const QString &text)
{
    if (findText_ == text)
        return;
    findText_ = text;
    rebuildFindMatches();
}

void TextFileEditor::rebuildFindMatches()
{
    findMatches_.clear();
    if (!findText_.isEmpty()) {
        const auto text = editorText();
        const QStringMatcher matcher(findText_, Qt::CaseInsensitive);
        for (qsizetype start = 0; (start = matcher.indexIn(text, start)) >= 0; start += findText_.size())
            findMatches_.append(int(start));
    }
    updateSelections();
    emit findMatchesChanged();
}

QTextCursor TextFileEditor::findCursor(int position) const
{
    QTextCursor cursor(document());
    cursor.setPosition(position);
    cursor.setPosition(position + int(findText_.size()), QTextCursor::KeepAnchor);
    return cursor;
}

void TextFileEditor::stepFindMatch(bool backwards)
{
    if (findMatches_.isEmpty())
        return;
    const auto cursor = textCursor();
    const auto begin = findMatches_.cbegin();
    const auto end = findMatches_.cend();
    const int position = backwards ? cursor.selectionStart() : cursor.selectionEnd();
    auto next = std::lower_bound(begin, end, position);
    if (backwards)
        next = next == begin ? end - 1 : next - 1;
    else if (next == end)
        next = begin;
    setTextCursor(findCursor(*next));
    ensureCursorVisible();
}

void TextFileEditor::setBaseSelections(QList<QTextEdit::ExtraSelection> selections)
{
    baseSelections_ = std::move(selections);
    updateSelections();
}

void TextFileEditor::updateSelections()
{
    auto selections = baseSelections_;
    auto begin = findMatches_.cbegin();
    // Scrollbar signals can arrive before contentsChanged during a document replacement.
    const auto lastValidMatch = document()->characterCount() - 1 - findText_.size();
    auto end = std::upper_bound(begin, findMatches_.cend(), lastValidMatch);
    if (findMatches_.size() > maxFullFindHighlights) {
        // Keep every match navigable, while formatting only visible blocks for dense searches.
        const auto first = firstVisibleBlock();
        const auto last = cursorForPosition(QPoint(0, viewport()->height())).block();
        begin = std::lower_bound(begin, end, first.position() - findText_.size() + 1);
        end = std::lower_bound(begin, end, last.position() + last.length());
    }
    if (begin != end) {
        const bool dark = palette().color(QPalette::Base).lightness() < 128;
        QTextCharFormat highlight;
        highlight.setBackground(QColor(dark ? "#6b5018" : "#ffe29c"));
        for (auto match = begin; match != end; ++match)
            selections.append({findCursor(*match), highlight});
    }
    setExtraSelections(selections);
}

void TextFileEditor::changeEvent(QEvent *event)
{
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && !findMatches_.isEmpty())
        updateSelections();
}

void TextFileEditor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    if (findMatches_.size() > maxFullFindHighlights)
        updateSelections();
}
