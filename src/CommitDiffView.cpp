#include "CommitDiffView.h"

#include "CommitDiffSource.h"
#include "DiffWordHighlights.h"
#include "GitFileList.h"
#include "GitStatus.h"
#include "TextFileEditor.h"

#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

namespace {
constexpr qsizetype maxLines = 30'000;
constexpr qsizetype workingFileLimit = 1024 * 1024;
constexpr int restoreGutterWidth = 24;

class DiffModeSelector final : public QComboBox
{
public:
    using QComboBox::QComboBox;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStyleOptionComboBox option;
        initStyleOption(&option);
        const auto arrow = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, this);
        option.subControls &= ~QStyle::SC_ComboBoxArrow;
        const QPointF center = QRectF(arrow).center();
        const auto role = isEnabled() ? QPalette::ButtonText : QPalette::PlaceholderText;
        QStylePainter painter(this);
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().color(role), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPolyline(QPolygonF{center + QPointF(-4, -2), center + QPointF(0, 2), center + QPointF(4, -2)});
    }
};

enum class Change { None, Added, Removed, Padding };

struct Line {
    QString text;
    int oldNumber = 0;
    int newNumber = 0;
    Change change = Change::None;
    QList<DiffWordHighlights::Span> changedWords;
};

class DiffEditor final : public TextFileEditor
{
public:
    enum Gutter { Before, After, Unified };

    DiffEditor(Gutter side, QWidget *parent)
        : TextFileEditor(parent), side_(side), gutter_(new QWidget(this))
    {
        setReadOnly(true);
        setUndoRedoEnabled(false);
        setLineWrapMode(NoWrap);
        const auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        setFont(font);
        setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        connect(this, &QPlainTextEdit::updateRequest, gutter_, [this] {
            gutter_->update();
            updateRestoreButtons();
        });
        connect(this, &QPlainTextEdit::blockCountChanged, this, [this] {
            if (fileDocument_)
                updateNumberWidth(blockCount());
        });
        gutter_->installEventFilter(this);
        updateGutter();
    }

    void setLines(QList<Line> lines, bool replaceDocument = true)
    {
        lines_ = std::move(lines);
        fileDocument_ = !replaceDocument;
        if (replaceDocument) {
            QStringList text;
            text.reserve(lines_.size());
            int biggestNumber = 0;
            for (const auto &line : lines_) {
                text.append(line.text);
                biggestNumber = std::max({biggestNumber, line.oldNumber, line.newNumber});
            }
            setPlainText(text.join(QLatin1Char('\n')));
            updateNumberWidth(biggestNumber);
        } else {
            updateNumberWidth(blockCount());
        }
        updateHighlights();
        gutter_->update();
    }

    int topLine() const { return firstVisibleBlock().blockNumber(); }

    void enableRestoreGutter(std::function<void(qsizetype)> restore)
    {
        restoreHunk_ = std::move(restore);
        restoreGutter_ = new QWidget(this);
        updateGutter();
    }

    void setRestoreRows(QList<int> rows)
    {
        restoreRows_ = std::move(rows);
        updateRestoreButtons();
    }

    void jump(int line)
    {
        const auto block = document()->findBlockByNumber(line);
        if (!block.isValid())
            return;
        setTextCursor(QTextCursor(block));
        centerCursor();
    }

protected:
    void changeEvent(QEvent *event) override
    {
        TextFileEditor::changeEvent(event);
        if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
            updateHighlights();
            gutter_->update();
        }
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == gutter_ && event->type() == QEvent::Paint) {
            paintGutter();
            return true;
        }
        return TextFileEditor::eventFilter(watched, event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        TextFileEditor::resizeEvent(event);
        updateGutter();
    }

private:
    void updateHighlights()
    {
        QList<QTextEdit::ExtraSelection> selections;
        for (auto block = document()->begin(); block.isValid() && block.blockNumber() < lines_.size();
             block = block.next()) {
            const auto &line = lines_.at(block.blockNumber());
            if (line.change == Change::None)
                continue;
            QTextEdit::ExtraSelection selection;
            selection.cursor = QTextCursor(block);
            selection.format.setProperty(QTextFormat::FullWidthSelection, true);
            selection.format.setBackground(background(line.change));
            selections.append(selection);

            if (line.changedWords.isEmpty() || block.length() - 1 != line.text.size())
                continue;
            const auto ink = changedWordBackground(line.change);
            for (const auto &word : line.changedWords) {
                QTextEdit::ExtraSelection accent;
                accent.cursor = QTextCursor(block);
                accent.cursor.setPosition(block.position() + word.start);
                accent.cursor.setPosition(block.position() + word.start + word.length, QTextCursor::KeepAnchor);
                accent.format.setBackground(ink);
                selections.append(accent);
            }
        }
        setExtraSelections(selections);
    }

    QColor background(Change change) const
    {
        const bool dark = palette().color(QPalette::Base).lightness() < 128;
        switch (change) {
        case Change::Added: return QColor(dark ? "#203e36" : "#d8f2e1");
        case Change::Removed: return QColor(dark ? "#482e39" : "#f9dce0");
        case Change::Padding: return QColor(dark ? "#202836" : "#eceff2");
        case Change::None: return palette().color(QPalette::AlternateBase);
        }
        return {};
    }

    QColor changedWordBackground(Change change) const
    {
        const bool dark = palette().color(QPalette::Base).lightness() < 128;
        return QColor(change == Change::Added ? (dark ? "#30624b" : "#a5e2bd")
                                               : (dark ? "#713e49" : "#f5b5c0"));
    }

    int numberWidth() const
    {
        return (digits_ + 1) * fontMetrics().horizontalAdvance(QLatin1Char('0')) + 6;
    }

    void updateNumberWidth(int biggestNumber)
    {
        const int digits = qMax(2, int(QString::number(biggestNumber).size()));
        if (digits_ != digits) {
            digits_ = digits;
            updateGutter();
        }
    }

    void updateGutter()
    {
        const int width = side_ == Unified ? 2 * numberWidth() + 17 : numberWidth() + 1;
        setViewportMargins(width, 0, restoreGutter_ ? restoreGutterWidth : 0, 0);
        const auto inner = contentsRect();
        gutter_->setGeometry(inner.left(), inner.top(), width, viewport()->height());
        updateRestoreButtons();
    }

    void updateRestoreButtons()
    {
        if (!restoreGutter_)
            return;
        const auto area = viewport()->geometry();
        restoreGutter_->setGeometry(area.right() + 1, area.top(), restoreGutterWidth, area.height());

        qsizetype visible = 0;
        const auto first = std::lower_bound(restoreRows_.cbegin(), restoreRows_.cend(), topLine());
        for (auto row = first; row != restoreRows_.cend(); ++row) {
            const auto block = document()->findBlockByNumber(*row);
            if (!block.isValid())
                break;
            const auto bounds = blockBoundingGeometry(block).translated(contentOffset());
            const int top = qRound(bounds.top());
            if (top >= area.height())
                break;
            if (visible == restoreButtons_.size()) {
                auto *button = new QToolButton(restoreGutter_);
                button->setObjectName("restoreDiffHunk");
                button->setArrowType(Qt::RightArrow);
                button->setToolTip(tr("Restore this block from HEAD"));
                button->setAccessibleName(button->toolTip());
                connect(button, &QToolButton::clicked, this, [this, button] {
                    restoreHunk_(button->property("hunkIndex").toLongLong());
                });
                restoreButtons_.append(button);
            }
            auto *button = restoreButtons_.at(visible++);
            button->setProperty("hunkIndex", qsizetype(row - restoreRows_.cbegin()));
            button->setGeometry(1, top, restoreGutterWidth - 2, qMax(1, qRound(bounds.height())));
            button->show();
        }
        for (; visible < restoreButtons_.size(); ++visible)
            restoreButtons_.at(visible)->hide();
    }

    void paintGutter()
    {
        QPainter painter(gutter_);
        painter.fillRect(gutter_->rect(), background(Change::None));
        const auto ink = palette().color(QPalette::PlaceholderText);
        const int width = numberWidth();
        int rowHeight = fontMetrics().height();
        for (auto block = firstVisibleBlock();
             block.isValid() && (fileDocument_ || block.blockNumber() < lines_.size());
             block = block.next()) {
            const auto bounds = blockBoundingGeometry(block).translated(contentOffset());
            const int top = qRound(bounds.top());
            if (top >= gutter_->height())
                break;
            rowHeight = qMax(1, qRound(bounds.bottom()) - top);
            const int number = block.blockNumber();
            const auto line = lines_.value(number);
            if (line.change != Change::None)
                painter.fillRect(0, top, gutter_->width(), rowHeight, background(line.change));
            painter.setPen(ink);
            const auto drawNumber = [&](int number, int x) {
                if (number > 0)
                    painter.drawText(QRect(x, top, width - 6, rowHeight),
                                     Qt::AlignRight | Qt::AlignVCenter, QString::number(number));
            };
            if (side_ != After)
                drawNumber(line.oldNumber, 0);
            if (side_ != Before)
                drawNumber(fileDocument_ ? number + 1 : line.newNumber, side_ == Unified ? width : 0);
            if (side_ == Unified && (line.change == Change::Added || line.change == Change::Removed)) {
                const bool added = line.change == Change::Added;
                painter.setPen(palette().color(QPalette::Base).lightness() < 128
                    ? QColor(added ? "#8de5a6" : "#ff9c9c") : QColor(added ? "#116425" : "#a42222"));
                painter.drawText(QRect(width * 2, top, 15, rowHeight),
                                 Qt::AlignCenter, added ? QStringLiteral("+") : QStringLiteral("−"));
            }
        }
        QColor divider = ink;
        divider.setAlpha(80);
        painter.setPen(divider);
        painter.drawLine(gutter_->width() - 1, 0, gutter_->width() - 1, gutter_->height());
    }

    Gutter side_;
    QWidget *gutter_;
    QWidget *restoreGutter_ = nullptr;
    std::function<void(qsizetype)> restoreHunk_;
    QList<int> restoreRows_;
    QList<QToolButton *> restoreButtons_;
    QList<Line> lines_;
    int digits_ = 2;
    bool fileDocument_ = false;
};

QStringList fileLines(const QString &text)
{
    if (text.isEmpty())
        return {};
    auto lines = text.split(QLatin1Char('\n'));
    if (lines.last().isEmpty())
        lines.removeLast();
    if (lines.first().startsWith(QChar::ByteOrderMark))
        lines.first().removeFirst();
    for (auto &line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        // A stray carriage return or Unicode separator would create an extra QTextBlock.
        line.replace(QLatin1Char('\r'), QChar(0x240d));
        line.replace(QChar(0x2028), QChar(0x2424));
        line.replace(QChar(0x2029), QChar(0x2424));
    }
    return lines;
}

QString comparisonText(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    return text.replace(u'\r', u'\n');
}

QString editorComparisonText(const QString &text)
{
    auto normalized = comparisonText(text);
    if (normalized.startsWith(QChar::ByteOrderMark))
        normalized.removeFirst();
    return normalized;
}

QList<int> lineOffsets(QStringView text)
{
    QList<int> offsets{0};
    for (qsizetype lineEnd = text.indexOf(u'\n'); lineEnd >= 0; lineEnd = text.indexOf(u'\n', lineEnd + 1))
        offsets.append(int(lineEnd + 1));
    if (offsets.last() != text.size())
        offsets.append(int(text.size()));
    return offsets;
}

struct RestoreRange {
    int start;
    int end;
    QString text;
};

struct Comparison {
    QList<Line> before;
    QList<Line> after;
    QList<Line> unified;
    QList<int> alignedChanges;
    QList<int> unifiedChanges;
};

bool compare(const QStringList &before, const QStringList &after,
             const QList<CommitDiffSource::Hunk> &hunks, Comparison &out)
{
    int oldLine = 0;
    int newLine = 0;
    const auto appendContext = [&](int end) {
        while (oldLine < end) {
            const auto &oldText = before.at(oldLine);
            const auto &newText = after.at(newLine);
            out.before.append({oldText, oldLine + 1});
            out.after.append({newText, 0, newLine + 1});
            out.unified.append({newText, oldLine + 1, newLine + 1});
            ++oldLine;
            ++newLine;
        }
    };

    for (const auto &hunk : hunks) {
        const int oldStart = hunk.oldStart - (hunk.oldCount > 0 ? 1 : 0);
        const int newStart = hunk.newStart - (hunk.newCount > 0 ? 1 : 0);
        if (oldStart < oldLine || newStart < newLine || oldStart > before.size() || newStart > after.size()
            || oldStart - oldLine != newStart - newLine || hunk.oldCount > before.size() - oldStart
            || hunk.newCount > after.size() - newStart)
            return false;
        appendContext(oldStart);
        out.alignedChanges.append(int(out.before.size()));
        out.unifiedChanges.append(int(out.unified.size()));
        const auto words = DiffWordHighlights::compare(before, oldStart, hunk.oldCount,
                                                       after, newStart, hunk.newCount);
        for (int i = 0; i < hunk.oldCount; ++i) {
            Line line{before.at(oldLine), oldLine + 1, 0, Change::Removed, words.before.at(i)};
            out.before.append(line);
            out.unified.append(line);
            ++oldLine;
        }
        for (int i = 0; i < hunk.newCount; ++i) {
            Line line{after.at(newLine), 0, newLine + 1, Change::Added, words.after.at(i)};
            out.after.append(line);
            out.unified.append(line);
            ++newLine;
        }
        while (out.before.size() < out.after.size())
            out.before.append({{}, 0, 0, Change::Padding});
        while (out.after.size() < out.before.size())
            out.after.append({{}, 0, 0, Change::Padding});
    }
    if (before.size() - oldLine != after.size() - newLine)
        return false;
    appendContext(int(before.size()));
    return true;
}

class DiffPage final : public QWidget
{
    Q_OBJECT

public:
    DiffPage(QWidget *parent, const QString &repository, const QString &hash, const QString &subject,
             const QByteArray &workingPath = {})
        : QWidget(parent), source_(repository, this), working_(hash.isEmpty())
    {
        setObjectName("commitDiffView");
        setProperty("commitHash", hash);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        auto *toolbar = new QHBoxLayout;
        toolbar->setSpacing(6);
        previous_ = changeButton(tr("Previous change (Shift+F7)"), Qt::UpArrow, Qt::SHIFT | Qt::Key_F7, -1);
        next_ = changeButton(tr("Next change (F7)"), Qt::DownArrow, Qt::Key_F7, 1);
        toolbar->addWidget(previous_);
        toolbar->addWidget(next_);
        mode_ = new DiffModeSelector(this);
        mode_->setObjectName("diffMode");
        mode_->setAccessibleName(tr("Diff view mode"));
        mode_->addItems({tr("Side by Side"), tr("Unified")});
        mode_->setFixedHeight(28);
        toolbar->addWidget(mode_);
        if (working_) {
            toolbar->addStretch(1);
        } else {
            toolbar->addSpacing(6);
            auto *description = new QLabel(hash.left(12) + QStringLiteral("  ") + subject, this);
            description->setTextFormat(Qt::PlainText);
            description->setToolTip(hash + QLatin1Char('\n') + subject);
            description->setWordWrap(true);
            description->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            toolbar->addWidget(description, 1);
        }
        layout->addLayout(toolbar);

        auto *split = new QSplitter(Qt::Horizontal, this);
        split->setChildrenCollapsible(false);
        auto *sidebar = new QWidget(split);
        auto *sidebarLayout = new QVBoxLayout(sidebar);
        sidebarLayout->setContentsMargins(0, 0, 0, 0);
        sidebarLayout->setSpacing(4);
        files_ = new QTreeWidget(sidebar);
        files_->setObjectName("diffFiles");
        files_->setAccessibleName(tr("Changed files"));
        files_->setHeaderLabels({tr("Changed files")});
        files_->setRootIsDecorated(false);
        files_->setUniformRowHeights(true);
        GitFileList::useStatusColors(*files_);
        files_->setTextElideMode(Qt::ElideMiddle);
        sidebar->setMinimumWidth(working_ ? 170 : 130);
        if (working_) {
            files_->setHeaderHidden(true);
            auto *headerWidget = new QWidget(sidebar);
            headerWidget->setFixedHeight(26);
            auto *header = new QHBoxLayout(headerWidget);
            header->setContentsMargins(4, 0, 0, 0);
            auto *label = new QLabel(tr("Changed files"), headerWidget);
            label->setObjectName("muted");
            header->addWidget(label, 1);
            fileAction_ = new QToolButton(headerWidget);
            fileAction_->setObjectName("workingFileAction");
            fileAction_->setFixedHeight(26);
            fileAction_->hide();
            header->addWidget(fileAction_);
            sidebarLayout->addWidget(headerWidget);
            connect(fileAction_, &QToolButton::clicked, this, &DiffPage::applyFileAction);
        }
        sidebarLayout->addWidget(files_, 1);
        split->addWidget(sidebar);

        auto *view = new QWidget(split);
        auto *viewLayout = new QVBoxLayout(view);
        viewLayout->setContentsMargins(0, 0, 0, 0);
        filePath_ = new QLabel(view);
        filePath_->setTextFormat(Qt::PlainText);
        filePath_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        filePath_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        viewLayout->addWidget(filePath_);
        status_ = new QLabel(view);
        status_->setTextFormat(Qt::PlainText);
        status_->setWordWrap(true);
        status_->setObjectName("muted");
        viewLayout->addWidget(status_);
        if (working_) {
            autosaveStatus_ = new QLabel(view);
            autosaveStatus_->setTextFormat(Qt::PlainText);
            autosaveStatus_->setWordWrap(true);
            autosaveStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
            autosaveStatus_->setObjectName("autosaveStatus");
            autosaveStatus_->hide();
            viewLayout->addWidget(autosaveStatus_);
        }
        pages_ = new QStackedWidget(view);
        auto *paired = new QSplitter(Qt::Horizontal, pages_);
        paired->setChildrenCollapsible(false);
        before_ = addPane(paired, DiffEditor::Before, "diffBefore", working_ ? tr("HEAD") : tr("Before"));
        after_ = addPane(paired, DiffEditor::After, "diffAfter", working_ ? tr("Working Tree") : tr("After"));
        if (working_)
            before_->enableRestoreGutter([this](qsizetype index) { restoreHunk(index); });
        paired->setSizes({420, 420});
        pages_->addWidget(paired);
        unified_ = new DiffEditor(DiffEditor::Unified, pages_);
        unified_->setObjectName("diffUnified");
        unified_->setAccessibleName(tr("Unified diff"));
        pages_->addWidget(unified_);
        viewLayout->addWidget(pages_, 1);
        split->addWidget(view);
        split->setSizes({265, 835});
        split->setStretchFactor(1, 1);
        layout->addWidget(split, 1);

        connect(before_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int position) {
            synchronizeScroll(after_, afterRows_.value(position, position));
        });
        connect(after_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int position) {
            synchronizeScroll(before_, beforeRows_.value(position, position));
        });
        connect(mode_, &QComboBox::currentIndexChanged, this, [this](int index) {
            pages_->setCurrentIndex(index);
            if (currentChange_ >= 0)
                goToChange(currentChange_);
        });
        connect(files_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item, QTreeWidgetItem *previous) {
            if (!item) {
                updateFileAction();
                return;
            }
            if (auto *editor = workingEditor(); editor && !editor->flushAutosave()) {
                const QSignalBlocker blocked(files_);
                files_->setCurrentItem(previous);
                return;
            }
            showFile(item);
            updateFileAction();
        });

        if (working_) {
            worktreeRefresh_.setInterval(2000);
            connect(&worktreeRefresh_, &QTimer::timeout, this, [this] {
                if (!source_.workingFileActionInProgress() && !after_->document()->isModified() &&
                    !compareEdited_.isActive())
                    source_.refreshWorkingFiles(selectedWorkingPath());
            });
            connect(&source_, &CommitDiffSource::workingStateReady, this,
                    [this, workingPath](const CommitDiffSource::WorkingState &state) {
                updateWorkingState(state, workingPath);
            });
            connect(&source_, &CommitDiffSource::workingFileActionFinished, this, [this](const QString &error) {
                files_->setEnabled(true);
                pages_->setEnabled(true);
                refreshRequired_ = true;
                source_.loadWorkingFiles(selectedWorkingPath());
                if (!error.isEmpty()) {
                    auto *message = new QMessageBox(QMessageBox::Warning, tr("Could not apply file action"),
                                                    error, QMessageBox::Ok, this);
                    message->setTextFormat(Qt::PlainText);
                    message->setAttribute(Qt::WA_DeleteOnClose);
                    message->open();
                }
            });
            compareEdited_.setSingleShot(true);
            compareEdited_.setInterval(180);
            connect(&compareEdited_, &QTimer::timeout, this, &DiffPage::compareWorkingEdit);
            connect(after_, &QPlainTextEdit::textChanged, this, [this] {
                if (!editingFile_ || updatingEditors_)
                    return;
                clearRestoreActions();
                // Keep the document and its undo stack. Recompute only comparison metadata.
                after_->setLines({}, false);
                compareEdited_.start();
            });
            connect(after_->document(), &QTextDocument::modificationChanged, this, &DiffPage::updateFileLabel);
            connect(after_, &TextFileEditor::saved, this, [this] {
                worktreeSnapshot_ = source_.workingSnapshot(selectedWorkingPath());
            });
            connect(after_, &TextFileEditor::autosaveErrorChanged, this, [this](const QString &error) {
                autosaveStatus_->setText(error);
                autosaveStatus_->setVisible(!error.isEmpty());
            });
        }
        connect(&source_, &CommitDiffSource::filesReady, this, [this](const QList<CommitDiffSource::File> &files) {
            populateFiles(files);
            if (files_->topLevelItemCount() > 0) {
                files_->setCurrentItem(files_->topLevelItem(0));
            } else {
                status_->setText(tr("This commit has no changed files."));
            }
        });
        connect(&source_, &CommitDiffSource::previewReady, this, &DiffPage::showPreview);
        connect(&source_, &CommitDiffSource::failed, this, [this](const QString &error) {
            const bool duringRefresh = std::exchange(refreshingPreview_, false);
            refreshRequired_ |= duringRefresh;
            clearRestoreActions();
            if (!editingFile_ && !duringRefresh)
                clearEditors();
            status_->setText(error);
            status_->show();
            updateFileAction();
        });
        clearEditors();
        status_->setText(tr("Loading changed files…"));
        if (working_)
            source_.loadWorkingFiles(workingPath);
        else
            source_.loadFiles(hash);
    }

    TextFileEditor *workingEditor() const { return editingFile_ ? after_ : nullptr; }

    void reloadIfClean()
    {
        if (!editingFile_ || after_->document()->isModified())
            return;
        const auto previous = after_->editorText();
        const QScopedValueRollback updating(updatingEditors_, true);
        after_->reloadIfClean();
        if (after_->isReadOnly() && files_->currentItem())
            showFile(files_->currentItem());
        else if (previous != after_->editorText()) {
            clearRestoreActions();
            compareEdited_.start();
        }
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        reloadIfClean();
        if (working_)
            worktreeRefresh_.start();
    }

    void hideEvent(QHideEvent *event) override
    {
        worktreeRefresh_.stop();
        QWidget::hideEvent(event);
    }

private:
    QToolButton *changeButton(const QString &label, Qt::ArrowType arrow,
                              const QKeySequence &shortcut, int direction)
    {
        auto *button = new QToolButton(this);
        button->setObjectName("diffNavigation");
        button->setArrowType(arrow);
        button->setFixedSize(30, 28);
        button->setToolTip(label);
        button->setAccessibleName(label);
        auto *action = new QAction(this);
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(action);
        connect(action, &QAction::triggered, button, &QToolButton::click);
        connect(button, &QToolButton::clicked, this, [this, direction] {
            const bool unified = mode_->currentIndex() == 1;
            const auto &markers = unified ? unifiedChanges_ : alignedChanges_;
            if (markers.isEmpty())
                return;
            int target = currentChange_ + direction;
            const int top = unified ? unified_->topLine() : before_->topLine();
            if (currentChange_ < 0 || markers.at(currentChange_) < top) {
                const auto next = std::lower_bound(markers.cbegin(), markers.cend(), top);
                target = int(next - markers.cbegin()) - (direction < 0 ? 1 : 0);
            }
            goToChange(std::clamp(target, 0, int(markers.size()) - 1));
        });
        return button;
    }

    static DiffEditor *addPane(QSplitter *parent, DiffEditor::Gutter side,
                               const char *name, const QString &label)
    {
        auto *pane = new QWidget(parent);
        auto *layout = new QVBoxLayout(pane);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(3);
        auto *heading = new QLabel(label, pane);
        heading->setObjectName("muted");
        layout->addWidget(heading);
        auto *editor = new DiffEditor(side, pane);
        editor->setObjectName(QString::fromLatin1(name));
        editor->setAccessibleName(label);
        layout->addWidget(editor, 1);
        parent->addWidget(pane);
        return editor;
    }

    static QString displayPath(const CommitDiffSource::File &file)
    {
        if (file.oldPath.isEmpty() && file.newPath.isEmpty())
            return GitStatus::displayPath(file.workingPath);
        if (file.oldPath.isEmpty() || file.oldPath == file.newPath)
            return GitStatus::displayPath(file.newPath);
        if (file.newPath.isEmpty())
            return GitStatus::displayPath(file.oldPath);
        return GitStatus::displayPath(file.oldPath) + QStringLiteral(" → ") + GitStatus::displayPath(file.newPath);
    }

    QTreeWidgetItem *populateFiles(const QList<CommitDiffSource::File> &files, const QByteArray &selectedPath = {})
    {
        const QSignalBlocker blocked(files_);
        const int scroll = files_->verticalScrollBar()->value();
        files_->clear();
        changedFiles_ = files;
        QTreeWidgetItem *selected = nullptr;
        for (qsizetype index = 0; index < files.size(); ++index) {
            const auto &file = files.at(index);
            auto *item = new QTreeWidgetItem(files_, {displayPath(file)});
            item->setData(0, Qt::UserRole, index);
            GitFileList::setStatus(*item, *files_, file.status);
            if (!selectedPath.isEmpty() && file.workingPath == selectedPath)
                selected = item;
        }
        files_->setCurrentItem(selected);
        files_->verticalScrollBar()->setValue(scroll);
        return selected;
    }

    const CommitDiffSource::File &fileFor(const QTreeWidgetItem *item) const
    {
        return changedFiles_.at(item->data(0, Qt::UserRole).toLongLong());
    }

    QByteArray selectedWorkingPath() const
    {
        const auto *item = files_->currentItem();
        return item ? fileFor(item).workingPath : QByteArray{};
    }

    void updateFileAction()
    {
        if (!fileAction_)
            return;
        using Action = CommitDiffSource::WorkingFileAction;
        const auto *item = files_->currentItem();
        const auto action = item ? CommitDiffSource::workingFileAction(fileFor(item)) : Action::None;
        fileAction_->setVisible(action != Action::None);
        fileAction_->setEnabled(!source_.workingFileActionInProgress());
        if (action == Action::None)
            return;
        const bool restore = action == Action::RestoreDeleted;
        fileAction_->setText(restore ? tr("Restore") : tr("Delete"));
        const auto tooltip = restore ? tr("Restore this file from HEAD") : tr("Move this new file to Trash");
        fileAction_->setToolTip(tooltip);
        fileAction_->setAccessibleName(tooltip);
    }

    void applyFileAction()
    {
        const auto *item = files_->currentItem();
        if (!item || source_.workingFileActionInProgress())
            return;
        const auto file = fileFor(item);
        if (CommitDiffSource::workingFileAction(file) == CommitDiffSource::WorkingFileAction::None)
            return;
        if (auto *editor = workingEditor(); editor && !editor->flushAutosave())
            return;

        compareEdited_.stop();
        fileAction_->setEnabled(false);
        files_->setEnabled(false);
        pages_->setEnabled(false);
        source_.applyWorkingFileAction(file, worktreeHead_, worktreeSnapshot_);
    }

    void updateWorkingState(const CommitDiffSource::WorkingState &state, const QByteArray &initialPath)
    {
        if (source_.workingFileActionInProgress() || after_->document()->isModified() || compareEdited_.isActive())
            return;
        const bool firstLoad = !workingStateLoaded_;
        workingStateLoaded_ = true;
        auto *selected = files_->currentItem();
        const auto previousFile = selected ? fileFor(selected) : CommitDiffSource::File{};
        const bool filesChanged = firstLoad || changedFiles_ != state.files;
        if (filesChanged)
            selected = populateFiles(state.files, firstLoad ? initialPath : previousFile.workingPath);
        updateFileAction();

        const bool headChanged = worktreeHead_ != state.head;
        const bool diskChanged = worktreeSnapshot_ != state.selectedSnapshot;
        worktreeHead_ = state.head;
        worktreeSnapshot_ = state.selectedSnapshot;
        if (!selected) {
            if (filesChanged) {
                displayedPath_.clear();
                clearEditors();
                status_->setText(state.files.isEmpty() ? tr("No uncommitted changes.")
                    : tr("Select a changed file to compare."));
                status_->show();
            }
        } else if (firstLoad) {
            showFile(selected);
        } else if (headChanged || diskChanged || refreshRequired_ || previousFile != fileFor(selected)) {
            displayedPath_ = displayPath(fileFor(selected));
            updateFileLabel();
            clearRestoreActions();
            refreshingPreview_ = true;
            refreshRequired_ = false;
            source_.loadWorkingPreview(fileFor(selected));
        }
    }

    void clearEditors()
    {
        clearRestoreActions();
        editingFile_ = false;
        refreshingPreview_ = false;
        refreshRequired_ = false;
        compareEdited_.stop();
        currentChange_ = -1;
        alignedChanges_.clear();
        unifiedChanges_.clear();
        beforeRows_.clear();
        afterRows_.clear();
        for (auto *editor : {before_, after_, unified_}) {
            editor->clearFile();
            editor->setUndoRedoEnabled(false);
            editor->setLines({});
        }
        pages_->hide();
        mode_->setEnabled(false);
        mode_->setToolTip({});
        updateFileLabel();
        updateButtons();
    }

    void showFile(const QTreeWidgetItem *item)
    {
        const auto &file = fileFor(item);
        displayedPath_ = displayPath(file);
        clearEditors();
        status_->setText(tr("Loading file versions…"));
        status_->show();
        if (working_) {
            worktreeSnapshot_ = source_.workingSnapshot(file.workingPath);
            source_.loadWorkingPreview(file);
        } else {
            source_.loadPreview(file);
        }
    }

    void updateFileLabel()
    {
        const bool modified = editingFile_ && after_->document()->isModified();
        filePath_->setText(modified ? displayedPath_ + QStringLiteral(" •") : displayedPath_);
        filePath_->setToolTip(displayedPath_);
    }

    void showComparisonUnavailable(const QString &reason, bool refreshed)
    {
        clearRestoreActions();
        if (refreshed && !editingFile_) {
            clearEditors();
        } else if (refreshed) {
            const QScopedValueRollback scrolling(syncingScroll_, true);
            currentChange_ = -1;
            alignedChanges_.clear();
            unifiedChanges_.clear();
            beforeRows_.clear();
            afterRows_.clear();
            before_->setLines({});
            after_->setLines({}, false);
            unified_->setLines({});
            mode_->setCurrentIndex(0);
            mode_->setEnabled(false);
            updateButtons();
        }
        status_->setText(reason);
        status_->show();
    }

    void showPreview(const CommitDiffSource::Preview &preview)
    {
        const bool refreshed = std::exchange(refreshingPreview_, false);
        if (refreshed && after_->document()->isModified()) {
            refreshRequired_ = true;
            return;
        }
        if (refreshed && editingFile_) {
            const QScopedValueRollback updating(updatingEditors_, true);
            after_->reloadIfClean();
            if (after_->isReadOnly()) {
                editingFile_ = false;
                after_->clearFile();
                mode_->setToolTip({});
            } else if (preview.unavailableReason.isEmpty()) {
                if (editorComparisonText(preview.after) != after_->editorText()) {
                    refreshRequired_ = true;
                    return;
                }
                workingBefore_ = editorComparisonText(preview.before);
                workingHasHeadText_ = preview.hasHeadText;
            }
        }
        // If typing continued while Git was comparing snapshots, keep the newer buffer intact.
        if (!refreshed && editingFile_ && !preview.unavailableReason.isEmpty() && comparisonText_ != after_->editorText())
            return;
        if (!preview.unavailableReason.isEmpty()) {
            showComparisonUnavailable(preview.unavailableReason, refreshed);
            return;
        }
        if (!refreshed && editingFile_ && preview.after != after_->editorText())
            return;
        const auto before = fileLines(preview.before);
        const auto after = fileLines(preview.after);
        if (before.size() + after.size() > maxLines) {
            showComparisonUnavailable(tr("This file has too many lines to display."), refreshed);
            return;
        }
        Comparison comparison;
        if (!compare(before, after, preview.hunks, comparison)) {
            showComparisonUnavailable(tr("Could not align the two file versions."), refreshed);
            return;
        }
        const bool firstLoad = !editingFile_ && !refreshed;
        if (!editingFile_)
            prepareEditors(preview);
        alignedChanges_ = std::move(comparison.alignedChanges);
        unifiedChanges_ = std::move(comparison.unifiedChanges);
        {
            const QScopedValueRollback scrolling(syncingScroll_, true);
            const int afterPosition = after_->verticalScrollBar()->value();
            const int afterHorizontal = after_->horizontalScrollBar()->value();
            const int beforeHorizontal = before_->horizontalScrollBar()->value();
            const int unifiedPosition = unified_->verticalScrollBar()->value();
            const int unifiedHorizontal = unified_->horizontalScrollBar()->value();
            if (editingFile_) {
                QList<Line> actualLines;
                beforeRows_.clear();
                afterRows_.clear();
                for (int row = 0; row < comparison.after.size(); ++row) {
                    auto &line = comparison.after[row];
                    afterRows_.append(int(actualLines.size()));
                    if (line.newNumber > 0) {
                        beforeRows_.append(row);
                        actualLines.append(std::move(line));
                    }
                }
                beforeRows_.append(int(comparison.before.size()));
                after_->setLines(std::move(actualLines), false);
            } else {
                after_->setLines(std::move(comparison.after));
            }
            before_->setLines(std::move(comparison.before));
            unified_->setLines(std::move(comparison.unified));
            if (!firstLoad) {
                after_->verticalScrollBar()->setValue(afterPosition);
                before_->verticalScrollBar()->setValue(beforeRows_.value(afterPosition, afterPosition));
                unified_->verticalScrollBar()->setValue(unifiedPosition);
                after_->horizontalScrollBar()->setValue(afterHorizontal);
                before_->horizontalScrollBar()->setValue(beforeHorizontal);
                unified_->horizontalScrollBar()->setValue(unifiedHorizontal);
            }
        }
        status_->setText(tr("No text changes"));
        status_->setVisible(alignedChanges_.isEmpty());
        pages_->show();
        mode_->setEnabled(true);
        if (firstLoad && !alignedChanges_.isEmpty()) {
            goToChange(0);
        } else {
            currentChange_ = std::min(currentChange_, int(alignedChanges_.size()) - 1);
            updateButtons();
        }
        setRestoreActions(preview);
    }

    void clearRestoreActions()
    {
        restoreRanges_.clear();
        before_->setRestoreRows({});
    }

    void setRestoreActions(const CommitDiffSource::Preview &preview)
    {
        clearRestoreActions();
        if (!editingFile_ || !preview.hasHeadText || preview.hunks.isEmpty())
            return;
        const auto before = editorComparisonText(preview.before);
        const auto after = editorComparisonText(preview.after);
        if (after != after_->editorText() || before.count(u'\n') != preview.before.count(u'\n') ||
            after.count(u'\n') != preview.after.count(u'\n'))
            return;

        // Line boundaries include the newline when present, including at EOF.
        const auto oldOffsets = lineOffsets(before);
        const auto newOffsets = lineOffsets(after);
        QList<int> rows;
        for (qsizetype index = 0; index < preview.hunks.size(); ++index) {
            const auto &hunk = preview.hunks.at(index);
            const int oldLine = hunk.oldStart - (hunk.oldCount > 0 ? 1 : 0);
            const int newLine = hunk.newStart - (hunk.newCount > 0 ? 1 : 0);
            if (oldLine + hunk.oldCount >= oldOffsets.size() || newLine + hunk.newCount >= newOffsets.size())
                continue;
            const int start = newOffsets.at(newLine);
            const int end = newOffsets.at(newLine + hunk.newCount);
            const int oldStart = oldOffsets.at(oldLine);
            const auto text = QStringView(before).sliced(oldStart, oldOffsets.at(oldLine + hunk.oldCount) - oldStart);
            if (text == QStringView(after).sliced(start, end - start) || text.contains(QChar::ParagraphSeparator))
                continue;
            restoreRanges_.append({start, end, text.toString()});
            rows.append(alignedChanges_.at(index));
        }
        restoreRevision_ = after_->document()->revision();
        before_->setRestoreRows(std::move(rows));
    }

    void restoreHunk(qsizetype index)
    {
        if (!editingFile_ || after_->isReadOnly() || index < 0 || index >= restoreRanges_.size() ||
            after_->document()->revision() != restoreRevision_)
            return;
        if (source_.workingSnapshot(selectedWorkingPath()) != worktreeSnapshot_) {
            clearRestoreActions();
            refreshRequired_ = true;
            source_.refreshWorkingFiles(selectedWorkingPath());
            return;
        }

        const auto range = restoreRanges_.at(index);
        QTextCursor cursor(after_->document());
        cursor.setPosition(range.start);
        cursor.setPosition(range.end, QTextCursor::KeepAnchor);
        cursor.beginEditBlock();
        cursor.insertText(range.text);
        cursor.endEditBlock();
        after_->setTextCursor(cursor);
        after_->setFocus();
        after_->flushAutosave();
    }

    void prepareEditors(const CommitDiffSource::Preview &preview)
    {
        const auto &file = changedFiles_.at(files_->currentItem()->data(0, Qt::UserRole).toLongLong());
        const auto oldPath = file.oldPath.isEmpty() ? file.workingPath : file.oldPath;
        const auto newPath = file.newPath.isEmpty() ? file.workingPath : file.newPath;
        before_->setSyntaxFileName(QString::fromUtf8(oldPath));
        after_->setSyntaxFileName(QString::fromUtf8(newPath));
        unified_->setSyntaxFileName(QString::fromUtf8(newPath.isEmpty() ? oldPath : newPath));
        if (!working_ || file.workingPath.isEmpty())
            return;

        const auto relativePath = QString::fromUtf8(file.workingPath);
        if (relativePath.toUtf8() != file.workingPath)
            return;
        const QDir worktree(source_.workingRoot());
        const QString path = QDir::cleanPath(worktree.filePath(relativePath));
        const QFileInfo info(path);
        const QString pathInWorktree = worktree.relativeFilePath(path);
        if (pathInWorktree != relativePath || relativePath.startsWith(QStringLiteral("../")) ||
            QDir::isAbsolutePath(relativePath) || !info.isFile() || info.canonicalFilePath() != path)
            return;
        const QScopedValueRollback updating(updatingEditors_, true);
        const auto expected = preview.after.isNull() ? QStringLiteral("") : preview.after;
        const auto result = after_->openFile(path, workingFileLimit, expected);
        if (result != TextFileEditor::LoadResult::Ready) {
            after_->clearFile();
            after_->setSyntaxFileName(QString::fromUtf8(newPath));
            return;
        }
        editingFile_ = true;
        workingBefore_ = editorComparisonText(preview.before);
        workingHasHeadText_ = preview.hasHeadText;
        after_->setUndoRedoEnabled(true);
        after_->document()->setModified(false);
        mode_->setToolTip(tr("Edit the working file in Side by Side mode."));
        if (fileLines(after_->editorText()).size() != fileLines(preview.after).size())
            compareEdited_.start();
    }

    void compareWorkingEdit()
    {
        refreshRequired_ |= std::exchange(refreshingPreview_, false);
        CommitDiffSource::Preview preview;
        preview.before = workingBefore_;
        preview.after = after_->editorText();
        preview.hasHeadText = workingHasHeadText_;
        comparisonText_ = preview.after;
        if (preview.before.count(u'\n') + preview.after.count(u'\n') >= maxLines ||
            preview.after.toUtf8().size() > workingFileLimit) {
            status_->setText(tr("The edited file is too large to compare."));
            status_->show();
            return;
        }
        source_.compareWorkingText(std::move(preview));
    }

    void synchronizeScroll(DiffEditor *target, int position)
    {
        if (syncingScroll_)
            return;
        const QScopedValueRollback guard(syncingScroll_, true);
        target->verticalScrollBar()->setValue(position);
    }

    void goToChange(int index)
    {
        currentChange_ = index;
        if (mode_->currentIndex() == 0) {
            const QScopedValueRollback scrolling(syncingScroll_, true);
            const int line = alignedChanges_.at(index);
            before_->jump(line);
            after_->jump(afterRows_.value(line, line));
        } else {
            unified_->jump(unifiedChanges_.at(index));
        }
        updateButtons();
    }

    void updateButtons()
    {
        previous_->setEnabled(currentChange_ > 0);
        next_->setEnabled(currentChange_ + 1 < alignedChanges_.size());
    }

    CommitDiffSource source_;
    const bool working_;
    QList<CommitDiffSource::File> changedFiles_;
    QTreeWidget *files_ = nullptr;
    QComboBox *mode_ = nullptr;
    QToolButton *previous_ = nullptr;
    QToolButton *next_ = nullptr;
    QToolButton *fileAction_ = nullptr;
    QLabel *filePath_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *autosaveStatus_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    DiffEditor *before_ = nullptr;
    DiffEditor *after_ = nullptr;
    DiffEditor *unified_ = nullptr;
    QString displayedPath_;
    QString workingBefore_;
    QString comparisonText_;
    QByteArray worktreeHead_;
    QByteArray worktreeSnapshot_;
    QTimer compareEdited_;
    QTimer worktreeRefresh_;
    QList<int> beforeRows_;
    QList<int> afterRows_;
    QList<int> alignedChanges_;
    QList<int> unifiedChanges_;
    QList<RestoreRange> restoreRanges_;
    int restoreRevision_ = 0;
    int currentChange_ = -1;
    bool syncingScroll_ = false;
    bool editingFile_ = false;
    bool workingHasHeadText_ = false;
    bool updatingEditors_ = false;
    bool workingStateLoaded_ = false;
    bool refreshingPreview_ = false;
    bool refreshRequired_ = false;
};
}

QWidget *CommitDiffView::create(QWidget *parent, const QString &repository, const QString &hash,
                                const QString &subject)
{
    return new DiffPage(parent, repository, hash, subject);
}

QWidget *CommitDiffView::createWorking(QWidget *parent, const QString &repository, const QByteArray &path)
{
    return new DiffPage(parent, repository, {}, {}, path);
}

TextFileEditor *CommitDiffView::workingEditor(QWidget *page)
{
    auto *diff = qobject_cast<DiffPage *>(page);
    return diff ? diff->workingEditor() : nullptr;
}

bool CommitDiffView::flushAutosave(QWidget *page)
{
    auto *editor = workingEditor(page);
    return !editor || editor->flushAutosave();
}

void CommitDiffView::reloadIfClean(QWidget *page)
{
    if (auto *diff = qobject_cast<DiffPage *>(page))
        diff->reloadIfClean();
}

#include "CommitDiffView.moc"
