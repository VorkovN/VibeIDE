#include "Terminal.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QFontInfo>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QInputMethodEvent>

#include <algorithm>
#include <limits>
#include <utility>

namespace {
constexpr int Margin = 8;
constexpr int MaxHistory = 1500;
QString cellText(const VTermScreenCell &cell)
{
    if (!cell.chars[0])
        return QStringLiteral(" ");
    if (cell.chars[0] == std::numeric_limits<uint32_t>::max())
        return {};
    char32_t chars[VTERM_MAX_CHARS_PER_CELL];
    int length = 0;
    for (auto codepoint : cell.chars) {
        if (!codepoint)
            break;
        chars[length++] = static_cast<char32_t>(codepoint);
    }
    return QString::fromUcs4(chars, length);
}

VTermModifier modifiers(Qt::KeyboardModifiers keys)
{
    int value = VTERM_MOD_NONE;
    if (keys.testFlag(Qt::ShiftModifier))
        value |= VTERM_MOD_SHIFT;
    if (keys.testFlag(Qt::AltModifier))
        value |= VTERM_MOD_ALT;
    // Qt exchanges Control and Meta on macOS.
    if (keys.testFlag(Qt::MetaModifier))
        value |= VTERM_MOD_CTRL;
    return static_cast<VTermModifier>(value);
}

VTermKey terminalKey(int key)
{
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter: return VTERM_KEY_ENTER;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return VTERM_KEY_TAB;
    case Qt::Key_Backspace: return VTERM_KEY_BACKSPACE;
    case Qt::Key_Escape: return VTERM_KEY_ESCAPE;
    case Qt::Key_Up: return VTERM_KEY_UP;
    case Qt::Key_Down: return VTERM_KEY_DOWN;
    case Qt::Key_Left: return VTERM_KEY_LEFT;
    case Qt::Key_Right: return VTERM_KEY_RIGHT;
    case Qt::Key_Insert: return VTERM_KEY_INS;
    case Qt::Key_Delete: return VTERM_KEY_DEL;
    case Qt::Key_Home: return VTERM_KEY_HOME;
    case Qt::Key_End: return VTERM_KEY_END;
    case Qt::Key_PageUp: return VTERM_KEY_PAGEUP;
    case Qt::Key_PageDown: return VTERM_KEY_PAGEDOWN;
    default:
        if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
            return static_cast<VTermKey>(VTERM_KEY_FUNCTION(key - Qt::Key_F1 + 1));
        return VTERM_KEY_NONE;
    }
}

bool precedes(QPoint first, QPoint second)
{
    return std::pair{first.y(), first.x()} < std::pair{second.y(), second.x()};
}

int mouseButton(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton: return 1;
    case Qt::MiddleButton: return 2;
    case Qt::RightButton: return 3;
    default: return 0;
    }
}
}

Terminal::Terminal(const QString &directory, QWidget *parent)
    : QAbstractScrollArea(parent)
    , term_(vterm_new(24, 80))
    , state_(vterm_obtain_state(term_))
    , screen_(vterm_obtain_screen(term_))
{
    setObjectName("terminal");
    setAccessibleName(tr("Terminal"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    viewport()->setMouseTracking(true);
    auto terminalFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (!QFontInfo(terminalFont).fixedPitch())
        terminalFont.setFamily(QStringLiteral("Menlo"));
    terminalFont.setPointSize(12);
    setFont(terminalFont);
    cellWidth_ = std::max(1, fontMetrics().horizontalAdvance(QLatin1Char('M')));
    cellHeight_ = fontMetrics().height() + 2;

    vterm_set_utf8(term_, 1);
    vterm_output_set_callback(term_, [](const char *bytes, size_t length, void *user) {
        static_cast<Terminal *>(user)->pty_.write(QByteArray(bytes, qsizetype(length)));
    }, this);
    static const VTermScreenCallbacks callbacks{
        .damage = nullptr,
        .moverect = nullptr,
        .movecursor = [](VTermPos, VTermPos, int visible, void *user) {
            static_cast<Terminal *>(user)->cursorVisible_ = visible;
            return 1;
        },
        .settermprop = [](VTermProp prop, VTermValue *value, void *user) {
            auto *self = static_cast<Terminal *>(user);
            if (prop == VTERM_PROP_ALTSCREEN) {
                self->alternate_ = value->boolean;
                self->hasSelection_ = false;
            }
            if (prop == VTERM_PROP_MOUSE)
                self->mouseMode_ = value->number;
            return 1;
        },
        .bell = nullptr,
        .resize = nullptr,
        .sb_pushline = [](int cols, const VTermScreenCell *cells, void *user) {
            auto *self = static_cast<Terminal *>(user);
            self->history_.emplace_back(cells, cells + cols);
            if (self->history_.size() > MaxHistory)
                self->history_.pop_front();
            return 1;
        },
        .sb_popline = [](int cols, VTermScreenCell *cells, void *user) {
            auto &history = static_cast<Terminal *>(user)->history_;
            if (history.empty())
                return 0;
            const auto &line = history.back();
            for (int i = 0; i < cols; ++i) {
                if (i < int(line.size())) {
                    cells[i] = line[size_t(i)];
                } else {
                    cells[i] = {};
                    cells[i].width = 1;
                    cells[i].fg.type = VTERM_COLOR_DEFAULT_FG;
                    cells[i].bg.type = VTERM_COLOR_DEFAULT_BG;
                }
                if (i == cols - 1 && cells[i].width == 2)
                    cells[i].width = 1;
            }
            history.pop_back();
            return 1;
        },
        .sb_clear = [](void *user) {
            static_cast<Terminal *>(user)->history_.clear();
            return 1;
        },
    };
    vterm_screen_set_callbacks(screen_, &callbacks, this);
    vterm_screen_enable_altscreen(screen_, 1);
    vterm_screen_enable_reflow(screen_, true);
    vterm_screen_reset(screen_, 1);

    connect(verticalScrollBar(), &QScrollBar::valueChanged, viewport(),
            qOverload<>(&QWidget::update));
    connect(&pty_, &Pty::dataReceived, this, &Terminal::receive);
    connect(&pty_, &Pty::finished, this, [this] {
        receive("\r\n[Shell exited. Open a new terminal to continue.]\r\n");
    });
    if (!pty_.start(directory, rows_, cols_))
        receive("\r\n" + pty_.errorString().toUtf8() + "\r\n");
}

Terminal::~Terminal()
{
    disconnect(&pty_, nullptr, this, nullptr);
    vterm_free(term_);
}

QString Terminal::workingDirectory() const
{
    return pty_.workingDirectory();
}

void Terminal::receive(const QByteArray &bytes)
{
    const bool followOutput = verticalScrollBar()->value() == verticalScrollBar()->maximum();
    if (!selecting_)
        hasSelection_ = false;
    vterm_input_write(term_, bytes.constData(), size_t(bytes.size()));
    vterm_screen_flush_damage(screen_);
    syncScroll(followOutput || alternate_);
}

void Terminal::syncScroll(bool followOutput)
{
    auto *bar = verticalScrollBar();
    bar->setRange(0, alternate_ ? 0 : int(history_.size()));
    bar->setPageStep(rows_);
    if (followOutput)
        bar->setValue(bar->maximum());
    viewport()->update();
}

void Terminal::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    const int rows = std::clamp((viewport()->height() - 2 * Margin) / cellHeight_, 2, 256);
    const int cols = std::clamp((viewport()->width() - 2 * Margin) / cellWidth_, 8, 320);
    if (rows == rows_ && cols == cols_)
        return;
    const bool followOutput = verticalScrollBar()->value() == verticalScrollBar()->maximum();
    hasSelection_ = false;
    rows_ = rows;
    cols_ = cols;
    vterm_set_size(term_, rows_, cols_);
    vterm_screen_flush_damage(screen_);
    pty_.resize(rows_, cols_);
    syncScroll(followOutput);
}

bool Terminal::readCell(int row, int col, VTermScreenCell &cell) const
{
    const int scrollback = alternate_ ? 0 : int(history_.size());
    if (row < scrollback) {
        const auto &line = history_[size_t(row)];
        if (col >= int(line.size()))
            return false;
        cell = line[size_t(col)];
        return true;
    }
    return vterm_screen_get_cell(screen_, {row - scrollback, col}, &cell);
}

QColor Terminal::color(VTermColor value) const
{
    const auto base = palette().color(QPalette::Base);
    if (VTERM_COLOR_IS_DEFAULT_FG(&value))
        return palette().color(QPalette::Text);
    if (VTERM_COLOR_IS_DEFAULT_BG(&value))
        return base;
    if (VTERM_COLOR_IS_INDEXED(&value) && value.indexed.idx < 16) {
        static const QColor darkColors[]{
            "#262b36", "#ff7d92", "#9ed590", "#edc786",
            "#8cadff", "#d4a0ff", "#80d6d9", "#d5deee",
            "#687488", "#ffa2ae", "#b7e6a8", "#ffe0a4",
            "#b5caff", "#e2bcff", "#a1e7ee", "#ffffff",
        };
        static const QColor lightColors[]{
            "#202734", "#ad273e", "#287044", "#835500",
            "#2556a1", "#803a9b", "#136b78", "#e2e9f3",
            "#5a6373", "#c9314e", "#317a50", "#905d00",
            "#3464b2", "#9351a8", "#197787", "#f7f9fc",
        };
        return (base.lightness() < 128 ? darkColors : lightColors)[value.indexed.idx];
    }
    vterm_screen_convert_color_to_rgb(screen_, &value);
    return QColor(value.rgb.red, value.rgb.green, value.rgb.blue);
}

void Terminal::paintEvent(QPaintEvent *)
{
    QPainter painter(viewport());
    const auto base = palette().color(QPalette::Base);
    painter.fillRect(viewport()->rect(), base);
    const int top = verticalScrollBar()->value();
    QPoint start = anchor_;
    QPoint end = selectionEnd_;
    if (precedes(end, start))
        std::swap(start, end);

    const int ascent = fontMetrics().ascent() + 1;
    const QFont normal = font();
    for (int row = 0; row < rows_; ++row) {
        for (int col = 0; col < cols_; ++col) {
            VTermScreenCell cell{};
            if (!readCell(top + row, col, cell) ||
                cell.chars[0] == std::numeric_limits<uint32_t>::max())
                continue;
            const QRect rect(Margin + col * cellWidth_, Margin + row * cellHeight_,
                             std::max(1, int(cell.width)) * cellWidth_, cellHeight_);
            const QPoint position(col, top + row);
            QColor foreground = color(cell.fg);
            QColor background = color(cell.bg);
            if (cell.attrs.reverse)
                std::swap(foreground, background);
            if (hasSelection_ && !precedes(position, start) && !precedes(end, position)) {
                foreground = palette().color(QPalette::HighlightedText);
                background = palette().color(QPalette::Highlight);
            }
            if (background != base)
                painter.fillRect(rect, background);
            if (cell.chars[0] && !cell.attrs.conceal) {
                QFont style = normal;
                style.setBold(cell.attrs.bold);
                style.setItalic(cell.attrs.italic);
                style.setUnderline(cell.attrs.underline);
                style.setStrikeOut(cell.attrs.strike);
                painter.setFont(style);
                painter.setPen(foreground);
                painter.drawText(rect.left(), rect.top() + ascent, cellText(cell));
            }
        }
    }
    if (cursorVisible_ && hasFocus() && top == verticalScrollBar()->maximum()) {
        VTermPos cursor{};
        vterm_state_get_cursorpos(state_, &cursor);
        painter.fillRect(QRect(Margin + cursor.col * cellWidth_,
                               Margin + (cursor.row + 1) * cellHeight_ - 2,
                               cellWidth_, 2), palette().color(QPalette::Link));
    }
}

bool Terminal::event(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange)
        viewport()->update();
    if (event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            keyPressEvent(static_cast<QKeyEvent *>(event));
            return true;
        }
    }
    return QAbstractScrollArea::event(event);
}

void Terminal::keyPressEvent(QKeyEvent *event)
{
    const auto keys = event->modifiers();
    const bool clipboardModifier = keys == Qt::ControlModifier;
    if (clipboardModifier && event->key() == Qt::Key_C) {
        copy();
        return;
    }
    if (clipboardModifier && event->key() == Qt::Key_V) {
        paste();
        return;
    }
    if (keys == Qt::ShiftModifier &&
        (event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown)) {
        const int direction = event->key() == Qt::Key_PageUp ? -1 : 1;
        verticalScrollBar()->setValue(verticalScrollBar()->value() + direction * rows_);
        return;
    }
    const auto mod = modifiers(keys);
    const VTermKey key = terminalKey(event->key());
    if (key == VTERM_KEY_NONE && event->text().isEmpty() && !(mod & VTERM_MOD_CTRL)) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    hasSelection_ = false;
    syncScroll(true);
    if (key != VTERM_KEY_NONE) {
        vterm_keyboard_key(term_, key, mod);
    } else if ((mod & VTERM_MOD_CTRL) && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) {
        vterm_keyboard_unichar(term_, 'a' + event->key() - Qt::Key_A, mod);
    } else {
        // Qt already applies Shift to printable text.
        auto textMod = static_cast<VTermModifier>(mod & ~VTERM_MOD_SHIFT);
        // Option can produce a Unicode character through the active keyboard layout.
        if (!event->text().isEmpty() && event->text().front().unicode() > 127)
            textMod = static_cast<VTermModifier>(textMod & ~VTERM_MOD_ALT);
        for (const auto codepoint : event->text().toUcs4())
            vterm_keyboard_unichar(term_, codepoint, textMod);
    }
    event->accept();
}

void Terminal::inputMethodEvent(QInputMethodEvent *event)
{
    if (!event->commitString().isEmpty()) {
        hasSelection_ = false;
        syncScroll(true);
        pty_.write(event->commitString().toUtf8());
    }
    event->accept();
}

void Terminal::focusInEvent(QFocusEvent *event)
{
    QAbstractScrollArea::focusInEvent(event);
    vterm_state_focus_in(state_);
    viewport()->update();
}

void Terminal::focusOutEvent(QFocusEvent *event)
{
    QAbstractScrollArea::focusOutEvent(event);
    vterm_state_focus_out(state_);
    viewport()->update();
}

QPoint Terminal::cellAt(const QPoint &pixel) const
{
    return {std::clamp((pixel.x() - Margin) / cellWidth_, 0, cols_ - 1),
            std::clamp((pixel.y() - Margin) / cellHeight_, 0, rows_ - 1) +
                verticalScrollBar()->value()};
}

void Terminal::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    const auto cell = cellAt(event->position().toPoint());
    if (mouseMode_ != VTERM_PROP_MOUSE_NONE && !event->modifiers().testFlag(Qt::ShiftModifier)) {
        vterm_mouse_move(term_, cell.y() - verticalScrollBar()->value(), cell.x(), modifiers(event->modifiers()));
        if (const int button = mouseButton(event->button()))
            vterm_mouse_button(term_, button, true, modifiers(event->modifiers()));
    } else if (event->button() == Qt::LeftButton) {
        anchor_ = selectionEnd_ = cell;
        selecting_ = true;
        hasSelection_ = false;
        viewport()->update();
    }
}

void Terminal::mouseMoveEvent(QMouseEvent *event)
{
    const auto cell = cellAt(event->position().toPoint());
    if (selecting_) {
        selectionEnd_ = cell;
        hasSelection_ = selectionEnd_ != anchor_;
        viewport()->update();
    } else if (mouseMode_ != VTERM_PROP_MOUSE_NONE && !event->modifiers().testFlag(Qt::ShiftModifier)) {
        vterm_mouse_move(term_, cell.y() - verticalScrollBar()->value(), cell.x(), modifiers(event->modifiers()));
    }
}

void Terminal::mouseReleaseEvent(QMouseEvent *event)
{
    if (selecting_ && event->button() == Qt::LeftButton) {
        selecting_ = false;
    } else if (mouseMode_ != VTERM_PROP_MOUSE_NONE && !event->modifiers().testFlag(Qt::ShiftModifier)) {
        if (const int button = mouseButton(event->button()))
            vterm_mouse_button(term_, button, false, modifiers(event->modifiers()));
    }
}

void Terminal::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y();
    if (mouseMode_ != VTERM_PROP_MOUSE_NONE && !event->modifiers().testFlag(Qt::ShiftModifier)) {
        const auto cell = cellAt(event->position().toPoint());
        const auto mod = modifiers(event->modifiers());
        vterm_mouse_move(term_, cell.y() - verticalScrollBar()->value(), cell.x(), mod);
        if (delta)
            vterm_mouse_button(term_, delta > 0 ? 4 : 5, true, mod);
    } else {
        const int pixels = event->pixelDelta().y();
        const int step = pixels ? pixels / cellHeight_ : delta / 40;
        verticalScrollBar()->setValue(verticalScrollBar()->value() - (step ? step : (pixels > 0) - (pixels < 0)));
    }
    event->accept();
}

void Terminal::contextMenuEvent(QContextMenuEvent *event)
{
    if (mouseMode_ != VTERM_PROP_MOUSE_NONE && !event->modifiers().testFlag(Qt::ShiftModifier))
        return;
    QMenu menu(this);
    menu.addAction(tr("Copy"), this, &Terminal::copy)->setEnabled(hasSelection_);
    menu.addAction(tr("Paste"), this, &Terminal::paste);
    menu.exec(event->globalPos());
}

void Terminal::copy()
{
    if (!hasSelection_)
        return;
    QPoint start = anchor_;
    QPoint end = selectionEnd_;
    if (precedes(end, start))
        std::swap(start, end);
    QString text;
    for (int row = start.y(); row <= end.y(); ++row) {
        QString line;
        for (int col = row == start.y() ? start.x() : 0;
             col <= (row == end.y() ? end.x() : cols_ - 1); ++col) {
            VTermScreenCell cell{};
            line += readCell(row, col, cell) ? cellText(cell) : QStringLiteral(" ");
        }
        if (row != end.y()) {
            while (line.endsWith(QLatin1Char(' ')))
                line.chop(1);
            line += QLatin1Char('\n');
        }
        text += line;
    }
    QApplication::clipboard()->setText(text);
}

void Terminal::paste()
{
    auto text = QApplication::clipboard()->text();
    if (text.isEmpty())
        return;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    hasSelection_ = false;
    syncScroll(true);
    vterm_keyboard_start_paste(term_);
    pty_.write(text.toUtf8());
    vterm_keyboard_end_paste(term_);
}
