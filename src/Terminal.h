#pragma once

#include "Pty.h"

#include <QAbstractScrollArea>
#include <QPoint>
#include <QString>

#include <deque>
#include <vector>
#include <vterm.h>

class Terminal final : public QAbstractScrollArea
{
public:
    explicit Terminal(const QString &directory, QWidget *parent = nullptr);
    ~Terminal() override;

    QString workingDirectory() const;

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void receive(const QByteArray &bytes);
    void syncScroll(bool followOutput);
    void copy();
    void paste();
    QPoint cellAt(const QPoint &pixel) const;
    bool readCell(int row, int col, VTermScreenCell &cell) const;
    QColor color(VTermColor color) const;

    Pty pty_;
    VTerm *term_;
    VTermState *state_;
    VTermScreen *screen_;
    std::deque<std::vector<VTermScreenCell>> history_;
    int rows_ = 24;
    int cols_ = 80;
    int cellWidth_;
    int cellHeight_;
    QPoint anchor_;
    QPoint selectionEnd_;
    bool selecting_ = false;
    bool hasSelection_ = false;
    bool cursorVisible_ = true;
    bool alternate_ = false;
    int mouseMode_ = VTERM_PROP_MOUSE_NONE;
};
