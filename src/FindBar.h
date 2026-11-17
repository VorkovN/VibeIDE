#pragma once

#include <QPointer>
#include <QWidget>

class QAction;
class QLabel;
class QLineEdit;
class QPropertyAnimation;
class TextFileEditor;

class FindBar final : public QWidget
{
    Q_OBJECT

public:
    explicit FindBar(QWidget *parent);

    void openFor(TextFileEditor *editor);
    void setEditor(TextFileEditor *editor);
    void hideSearch();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void animateOpen();
    void updateSearch();
    void updateMatchCount();

    QPointer<TextFileEditor> editor_;
    QMetaObject::Connection matchesChanged_;
    QLineEdit *input_;
    QLabel *matchCount_;
    QAction *escape_;
    QPropertyAnimation *reveal_ = nullptr;
};
