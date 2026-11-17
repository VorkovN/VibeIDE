#include "FindBar.h"

#include "Appearance.h"
#include "TextFileEditor.h"

#include <QAction>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPropertyAnimation>
#include <QSignalBlocker>
#include <QToolButton>

#include <limits>

namespace {
class SearchGlyph final : public QWidget
{
public:
    explicit SearchGlyph(QWidget *parent) : QWidget(parent) { setFixedSize(16, 16); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().color(QPalette::PlaceholderText), 1.35,
                            Qt::SolidLine, Qt::RoundCap));
        painter.drawEllipse(QPointF(6.5, 6.5), 3.75, 3.75);
        painter.drawLine(QPointF(9.4, 9.4), QPointF(13.1, 13.1));
    }
};
}

FindBar::FindBar(QWidget *parent)
    : QWidget(parent)
    , input_(new QLineEdit(this))
    , matchCount_(new QLabel(this))
    , escape_(new QAction(this))
{
    setObjectName("fileFindBar");
    setAttribute(Qt::WA_StyledBackground);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(9, 3, 4, 3);
    layout->setSpacing(7);
    layout->addWidget(new SearchGlyph(this));

    input_->setObjectName("fileFindInput");
    input_->setAccessibleName(tr("Find in file"));
    input_->setPlaceholderText(tr("Find in file"));
    input_->setMaxLength(std::numeric_limits<int>::max());
    input_->installEventFilter(this);
    layout->addWidget(input_, 1);

    matchCount_->setObjectName("fileFindCount");
    layout->addWidget(matchCount_);

    auto *closeButton = new QToolButton(this);
    closeButton->setObjectName("fileFindClose");
    closeButton->setText(QStringLiteral("×"));
    closeButton->setFixedSize(24, 24);
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

    if (isHidden())
        animateOpen();
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
    if (reveal_) {
        reveal_->stop();
        graphicsEffect()->setEnabled(false);
    }
    escape_->setEnabled(false);
    if (editor) {
        editor->setFindText({});
        if (editor->isVisible())
            editor->setFocus(Qt::ShortcutFocusReason);
    }
}

void FindBar::animateOpen()
{
    if (Appearance::reduceMotion())
        return;
    if (!reveal_) {
        auto *fade = new QGraphicsOpacityEffect(this);
        setGraphicsEffect(fade);
        reveal_ = new QPropertyAnimation(fade, "opacity", this);
        reveal_->setDuration(140);
        reveal_->setStartValue(0.0);
        reveal_->setEndValue(1.0);
        reveal_->setEasingCurve(QEasingCurve::OutCubic);
        connect(reveal_, &QPropertyAnimation::finished, fade, [fade] { fade->setEnabled(false); });
    }
    graphicsEffect()->setEnabled(true);
    reveal_->start();
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
