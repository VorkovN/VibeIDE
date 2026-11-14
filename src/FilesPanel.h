#pragma once

#include <QString>
#include <QWidget>

class QFileSystemModel;
class QLabel;
class QModelIndex;
class QPlainTextEdit;
class QTreeView;

class FilesPanel final : public QWidget
{
public:
    explicit FilesPanel(const QString &root, QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void showPreview(const QModelIndex &index);

    const QString root_;
    QFileSystemModel *model_ = nullptr;
    QLabel *path_;
    QTreeView *tree_;
    QPlainTextEdit *preview_;
};
