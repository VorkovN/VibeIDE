#include "CommitDiffView.h"

#include "CommitDiffSource.h"
#include "GitFileList.h"
#include "GitStatus.h"

#include <QAction>
#include <QComboBox>
#include <QFontDatabase>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBlock>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {
enum class Change { None, Added, Removed, Padding };

struct Line {
    QString text;
    int oldNumber = 0;
    int newNumber = 0;
    Change change = Change::None;
};

class DiffEditor final : public QPlainTextEdit
{
public:
    enum Gutter { Before, After, Unified };

    DiffEditor(Gutter side, QWidget *parent)
        : QPlainTextEdit(parent), side_(side), gutter_(new QWidget(this))
    {
        setReadOnly(true);
        setUndoRedoEnabled(false);
        setLineWrapMode(NoWrap);
        const auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        setFont(font);
        setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        connect(this, &QPlainTextEdit::updateRequest, gutter_, [this] { gutter_->update(); });
        gutter_->installEventFilter(this);
        updateGutter();
    }

    void setLines(QList<Line> lines)
    {
        lines_ = std::move(lines);
        QStringList text;
        text.reserve(lines_.size());
        int biggestNumber = 0;
        for (const auto &line : lines_) {
            text.append(line.text);
            biggestNumber = std::max({biggestNumber, line.oldNumber, line.newNumber});
        }
        digits_ = qMax(2, int(QString::number(biggestNumber).size()));
        updateGutter();
        setPlainText(text.join(QLatin1Char('\n')));
        updateHighlights();
    }

    int topLine() const { return firstVisibleBlock().blockNumber(); }

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
        QPlainTextEdit::changeEvent(event);
        if (event->type() == QEvent::PaletteChange) {
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
        return QPlainTextEdit::eventFilter(watched, event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QPlainTextEdit::resizeEvent(event);
        updateGutter();
    }

private:
    void updateHighlights()
    {
        QList<QTextEdit::ExtraSelection> selections;
        for (auto block = document()->begin(); block.isValid() && block.blockNumber() < lines_.size();
             block = block.next()) {
            const auto change = lines_.at(block.blockNumber()).change;
            if (change == Change::None)
                continue;
            QTextEdit::ExtraSelection selection;
            selection.cursor = QTextCursor(block);
            selection.format.setProperty(QTextFormat::FullWidthSelection, true);
            selection.format.setBackground(background(change));
            selections.append(selection);
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

    int numberWidth() const
    {
        return (digits_ + 1) * fontMetrics().horizontalAdvance(QLatin1Char('0')) + 6;
    }

    void updateGutter()
    {
        const int width = side_ == Unified ? 2 * numberWidth() + 17 : numberWidth() + 1;
        setViewportMargins(width, 0, 0, 0);
        const auto inner = contentsRect();
        gutter_->setGeometry(inner.left(), inner.top(), width, viewport()->height());
    }

    void paintGutter()
    {
        QPainter painter(gutter_);
        painter.fillRect(gutter_->rect(), background(Change::None));
        const auto ink = palette().color(QPalette::PlaceholderText);
        const int width = numberWidth();
        int rowHeight = fontMetrics().height();
        for (auto block = firstVisibleBlock(); block.isValid() && block.blockNumber() < lines_.size();
             block = block.next()) {
            const auto bounds = blockBoundingGeometry(block).translated(contentOffset());
            const int top = qRound(bounds.top());
            if (top >= gutter_->height())
                break;
            rowHeight = qMax(1, qRound(bounds.bottom()) - top);
            const auto &line = lines_.at(block.blockNumber());
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
                drawNumber(line.newNumber, side_ == Unified ? width : 0);
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
    QList<Line> lines_;
    int digits_ = 2;
};

QStringList fileLines(const QString &text)
{
    if (text.isEmpty())
        return {};
    auto lines = text.split(QLatin1Char('\n'));
    if (lines.last().isEmpty())
        lines.removeLast();
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
        for (int i = 0; i < hunk.oldCount; ++i) {
            Line line{before.at(oldLine), oldLine + 1, 0, Change::Removed};
            out.before.append(line);
            out.unified.append(line);
            ++oldLine;
        }
        for (int i = 0; i < hunk.newCount; ++i) {
            Line line{after.at(newLine), 0, newLine + 1, Change::Added};
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
public:
    DiffPage(QWidget *parent, const QString &repository, const QString &hash, const QString &subject,
             const QByteArray &workingPath = {})
        : QWidget(parent), source_(repository, this), working_(hash.isEmpty())
    {
        setObjectName("commitDiffView");
        setProperty("commitHash", hash);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        auto *title = new QHBoxLayout;
        auto *description = new QLabel(working_ ? tr("Current changes")
            : hash.left(12) + QStringLiteral("  ") + subject, this);
        description->setTextFormat(Qt::PlainText);
        if (!working_)
            description->setToolTip(hash + QLatin1Char('\n') + subject);
        description->setWordWrap(true);
        description->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        title->addWidget(description, 1);

        previous_ = changeButton(tr("Previous change (Shift+F7)"), QStyle::SP_ArrowUp, Qt::SHIFT | Qt::Key_F7, -1);
        next_ = changeButton(tr("Next change (F7)"), QStyle::SP_ArrowDown, Qt::Key_F7, 1);
        title->addWidget(previous_);
        title->addWidget(next_);
        mode_ = new QComboBox(this);
        mode_->setObjectName("diffMode");
        mode_->setAccessibleName(tr("Diff view mode"));
        mode_->addItems({tr("Side by Side"), tr("Unified")});
        title->addWidget(mode_);
        layout->addLayout(title);

        auto *split = new QSplitter(Qt::Horizontal, this);
        split->setChildrenCollapsible(false);
        files_ = new QTreeWidget(split);
        files_->setObjectName("diffFiles");
        files_->setAccessibleName(tr("Changed files"));
        files_->setHeaderLabels({tr("Changed files")});
        files_->setRootIsDecorated(false);
        files_->setUniformRowHeights(true);
        GitFileList::useStatusColors(*files_);
        files_->setTextElideMode(Qt::ElideMiddle);
        files_->setMinimumWidth(130);
        split->addWidget(files_);

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
        pages_ = new QStackedWidget(view);
        auto *paired = new QSplitter(Qt::Horizontal, pages_);
        paired->setChildrenCollapsible(false);
        before_ = addPane(paired, DiffEditor::Before, "diffBefore", working_ ? tr("HEAD") : tr("Before"));
        after_ = addPane(paired, DiffEditor::After, "diffAfter", working_ ? tr("Working Tree") : tr("After"));
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

        auto synchronize = [this](DiffEditor *from, DiffEditor *to) {
            auto *target = to->verticalScrollBar();
            connect(from->verticalScrollBar(), &QScrollBar::valueChanged, this, [this, target](int position) {
                if (syncingScroll_)
                    return;
                // Qt uses the target scrollbar's signal to move its editor viewport.
                const QScopedValueRollback guard(syncingScroll_, true);
                target->setValue(position);
            });
        };
        synchronize(before_, after_);
        synchronize(after_, before_);
        connect(mode_, &QComboBox::currentIndexChanged, this, [this](int index) {
            pages_->setCurrentIndex(index);
            if (currentChange_ >= 0)
                goToChange(currentChange_);
        });
        connect(files_, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem *item) { if (item) showFile(item); });
        connect(&source_, &CommitDiffSource::filesReady, this, [this, workingPath](const QList<CommitDiffSource::File> &files) {
            changedFiles_ = files;
            QTreeWidgetItem *requested = nullptr;
            for (qsizetype index = 0; index < files.size(); ++index) {
                const auto &file = files.at(index);
                const auto paths = displayPath(file);
                auto *item = new QTreeWidgetItem(files_, {paths});
                item->setData(0, Qt::UserRole, index);
                GitFileList::setStatus(*item, *files_, file.status);
                if (working_ && file.workingPath == workingPath)
                    requested = item;
            }
            if (requested) {
                files_->setCurrentItem(requested);
            } else if (working_) {
                status_->setText(tr("The selected file no longer has uncommitted changes."));
            } else if (files_->topLevelItemCount() > 0) {
                files_->setCurrentItem(files_->topLevelItem(0));
            } else {
                status_->setText(tr("This commit has no changed files."));
            }
        });
        connect(&source_, &CommitDiffSource::previewReady, this, &DiffPage::showPreview);
        connect(&source_, &CommitDiffSource::failed, this, [this](const QString &error) {
            clearEditors();
            status_->setText(error);
            status_->show();
        });
        clearEditors();
        status_->setText(tr("Loading changed files…"));
        if (working_)
            source_.loadWorkingFiles();
        else
            source_.loadFiles(hash);
    }

private:
    QToolButton *changeButton(const QString &label, QStyle::StandardPixmap icon,
                              const QKeySequence &shortcut, int direction)
    {
        auto *button = new QToolButton(this);
        button->setIcon(style()->standardIcon(icon));
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

    void clearEditors()
    {
        currentChange_ = -1;
        alignedChanges_.clear();
        unifiedChanges_.clear();
        for (auto *editor : {before_, after_, unified_})
            editor->setLines({});
        pages_->hide();
        mode_->setEnabled(false);
        updateButtons();
    }

    void showFile(const QTreeWidgetItem *item)
    {
        const auto &file = changedFiles_.at(item->data(0, Qt::UserRole).toLongLong());
        const auto paths = displayPath(file);
        filePath_->setText(paths);
        filePath_->setToolTip(paths);
        clearEditors();
        status_->setText(tr("Loading file versions…"));
        status_->show();
        if (working_)
            source_.loadWorkingPreview(file);
        else
            source_.loadPreview(file);
    }

    void showPreview(const CommitDiffSource::Preview &preview)
    {
        if (!preview.unavailableReason.isEmpty()) {
            status_->setText(preview.unavailableReason);
            return;
        }
        const auto before = fileLines(preview.before);
        const auto after = fileLines(preview.after);
        constexpr int maxLines = 30000;
        if (before.size() + after.size() > maxLines) {
            status_->setText(tr("This file has too many lines to display."));
            return;
        }
        Comparison comparison;
        if (!compare(before, after, preview.hunks, comparison)) {
            status_->setText(tr("Could not align the two file versions."));
            return;
        }
        alignedChanges_ = std::move(comparison.alignedChanges);
        unifiedChanges_ = std::move(comparison.unifiedChanges);
        before_->setLines(std::move(comparison.before));
        after_->setLines(std::move(comparison.after));
        unified_->setLines(std::move(comparison.unified));
        status_->setText(tr("No text changes"));
        status_->setVisible(alignedChanges_.isEmpty());
        pages_->show();
        mode_->setEnabled(true);
        if (!alignedChanges_.isEmpty())
            goToChange(0);
        else
            updateButtons();
    }

    void goToChange(int index)
    {
        currentChange_ = index;
        if (mode_->currentIndex() == 0) {
            before_->jump(alignedChanges_.at(index));
            after_->jump(alignedChanges_.at(index));
        } else {
            unified_->jump(unifiedChanges_.at(index));
        }
        updateButtons();
    }

    void updateButtons()
    {
        previous_->setEnabled(currentChange_ > 0);
        next_->setEnabled(currentChange_ >= 0 && currentChange_ + 1 < alignedChanges_.size());
    }

    CommitDiffSource source_;
    const bool working_;
    QList<CommitDiffSource::File> changedFiles_;
    QTreeWidget *files_ = nullptr;
    QComboBox *mode_ = nullptr;
    QToolButton *previous_ = nullptr;
    QToolButton *next_ = nullptr;
    QLabel *filePath_ = nullptr;
    QLabel *status_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    DiffEditor *before_ = nullptr;
    DiffEditor *after_ = nullptr;
    DiffEditor *unified_ = nullptr;
    QList<int> alignedChanges_;
    QList<int> unifiedChanges_;
    int currentChange_ = -1;
    bool syncingScroll_ = false;
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
