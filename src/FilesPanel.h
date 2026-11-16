#pragma once

#include <QPersistentModelIndex>
#include <QString>
#include <QWidget>

class QFileSystemModel;
class QLabel;
class QModelIndex;
class QTreeView;
class TextFileEditor;

class FilesPanel final : public QWidget
{
public:
    explicit FilesPanel(const QString &root, QWidget *parent = nullptr);
    bool flushAutosave();
    const QString &autosaveError() const;
    void reloadIfClean();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void openSelection(const QModelIndex &index);
    void updatePath();

    const QString root_;
    QFileSystemModel *model_ = nullptr;
    QLabel *path_;
    QTreeView *tree_;
    TextFileEditor *editor_;
    QPersistentModelIndex openedIndex_;
    QString openedPath_;
    QString relativePath_;
    int modelChanges_ = 0;
    bool changingSelection_ = false;
};
