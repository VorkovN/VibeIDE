#include "FindBar.h"

#include "TextFileEditor.h"

#include <QAction>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QToolButton>

#include <limits>

FindBar::FindBar(QWidget *parent)
    : QWidget(parent)
    , input_(new QLineEdit(this))
    , matchCount_(new QLabel(this))
    , escape_(new QAction(this))
{
    setObjectName("fileFindBar");
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    input_->setObjectName("fileFindInput");
    input_->setAccessibleName(tr("Find in file"));
    input_->setPlaceholderText(tr("Find in file"));
    input_->setMaxLength(std::numeric_limits<int>::max());
    input_->installEventFilter(this);
    layout->addWidget(input_, 1);

    matchCount_->setObjectName("muted");
    layout->addWidget(matchCount_);

    auto *closeButton = new QToolButton(this);
    closeButton->setText(QStringLiteral("×"));
    closeButton->setAccessibleName(tr("Close find"));
    closeButton->setToolTip(tr("Close find (Esc)"));
    layout->addWidget(closeButton);
    connect(closeButton, &QToolButton::clicked, this, &FindBar::hideSearch);

    escape_->setShortcut(QKeySequence(Qt::Key_Escape));
    escape_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    escape_->setEnabled(false);
    (parent ? parent : this)->addAction(escape_);
    connect(escape_, &QAction::triggered, this, &FindBar::hideSearch);

    connect(input_, &QLineEdit::textChanged, this, &FindBar::updateSearch);
    hide();
}

void FindBar::openFor(TextFileEditor *editor)
{
    if (!editor)
        return;

    if (!input_->hasFocus()) {
        auto selected = editor->textCursor().selectedText();
        if (!selected.isEmpty()) {
            const QSignalBlocker block(input_);
            input_->setText(selected.replace(QChar::ParagraphSeparator, u'\n'));
        }
    }

    show();
    escape_->setEnabled(true);
    setEditor(editor);
    input_->setFocus(Qt::ShortcutFocusReason);
    input_->selectAll();
}

void FindBar::setEditor(TextFileEditor *editor)
{
    if (editor_ != editor) {
        disconnect(matchesChanged_);
        if (editor_)
            editor_->setFindText({});
        editor_ = editor;
        if (editor_)
            matchesChanged_ = connect(editor_, &TextFileEditor::findMatchesChanged, this, &FindBar::updateMatchCount);
    }
    updateSearch();
}

void FindBar::hideSearch()
{
    if (isHidden())
        return;

    const auto editor = editor_;
    hide();
    escape_->setEnabled(false);
    if (editor) {
        editor->setFindText({});
        if (editor->isVisible())
            editor->setFocus(Qt::ShortcutFocusReason);
    }
}

bool FindBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == input_ && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (editor_)
                editor_->stepFindMatch(key->modifiers().testFlag(Qt::ShiftModifier));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FindBar::updateSearch()
{
    if (!isHidden() && editor_)
        editor_->setFindText(input_->text());
    updateMatchCount();
}

void FindBar::updateMatchCount()
{
    const auto count = editor_ ? editor_->findMatchCount() : 0;
    matchCount_->setText(count == 1 ? tr("1 match") : tr("%1 matches").arg(count));
    matchCount_->setVisible(!input_->text().isEmpty());
}
