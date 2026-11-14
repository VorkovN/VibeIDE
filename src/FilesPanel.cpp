#include "FilesPanel.h"

#include "TextFileEditor.h"

#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QScopedValueRollback>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTextDocument>
#include <QTreeView>
#include <QVBoxLayout>

namespace {
constexpr qsizetype previewLimit = 256 * 1024;

class ProjectFiles final : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        const auto index = sourceModel()->index(row, 0, parent);
        return index.data(QFileSystemModel::FileNameRole).toString() != QStringLiteral(".git");
    }
};
}

FilesPanel::FilesPanel(const QString &root, QWidget *parent)
    : QWidget(parent)
    , root_(root)
    , path_(new QLabel(this))
    , tree_(new QTreeView(this))
    , editor_(new TextFileEditor(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    auto *split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    layout->addWidget(split);

    tree_->setObjectName("directoryTree");
    tree_->setAccessibleName(tr("Project files"));
    tree_->setHeaderHidden(true);
    tree_->setUniformRowHeights(true);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    split->addWidget(tree_);

    auto *details = new QWidget(split);
    auto *detailLayout = new QVBoxLayout(details);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(4);
    path_->setTextFormat(Qt::PlainText);
    path_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    path_->hide();
    detailLayout->addWidget(path_);

    editor_->setObjectName("fileEditor");
    editor_->setAccessibleName(tr("File editor"));
    editor_->clearFile(tr("Select a file to open."));
    connect(editor_->document(), &QTextDocument::modificationChanged, this, &FilesPanel::updatePath);
    detailLayout->addWidget(editor_, 1);
    split->addWidget(details);
    split->setSizes({280, 720});
    split->setStretchFactor(1, 1);
}

bool FilesPanel::confirmClose()
{
    return editor_->confirmClose();
}

bool FilesPanel::save()
{
    return editor_->save();
}

void FilesPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (model_) {
        editor_->reloadIfClean();
        updatePath();
        return;
    }
    if (!QFileInfo(root_).isDir()) {
        editor_->clearFile(tr("The project folder is unavailable."));
        return;
    }

    model_ = new QFileSystemModel(this);
    model_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    model_->setOption(QFileSystemModel::DontUseCustomDirectoryIcons);
    auto *files = new ProjectFiles(this);
    files->setSourceModel(model_);
    // The view can select a neighboring row as the model removes the open file.
    const auto beginModelChange = [this] { ++modelChanges_; };
    connect(files, &QAbstractItemModel::rowsAboutToBeRemoved, this, beginModelChange);
    connect(files, &QAbstractItemModel::modelAboutToBeReset, this, beginModelChange);
    tree_->setModel(files);
    const auto endModelChange = [this] {
        if (!openedIndex_.isValid() && !openedPath_.isEmpty())
            tree_->selectionModel()->clear();
        --modelChanges_;
    };
    connect(files, &QAbstractItemModel::rowsRemoved, this, endModelChange);
    connect(files, &QAbstractItemModel::modelReset, this, endModelChange);
    tree_->setRootIndex(files->mapFromSource(model_->setRootPath(root_)));
    for (int column = 1; column < files->columnCount(); ++column)
        tree_->hideColumn(column);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this, &FilesPanel::openSelection);
}

void FilesPanel::openSelection(const QModelIndex &index)
{
    if (!index.isValid() || modelChanges_ || changingSelection_)
        return;

    const QScopedValueRollback guard(changingSelection_, true);
    const QPersistentModelIndex requested(index);
    const auto *proxy = static_cast<const QSortFilterProxyModel *>(tree_->model());
    auto info = model_->fileInfo(proxy->mapToSource(requested));
    if (info.filePath() == openedPath_) {
        openedIndex_ = requested;
        editor_->reloadIfClean();
        updatePath();
        return;
    }
    if (!confirmClose() || !requested.isValid()) {
        if (openedIndex_.isValid())
            tree_->setCurrentIndex(openedIndex_);
        else
            tree_->selectionModel()->clear();
        return;
    }

    info = model_->fileInfo(proxy->mapToSource(requested));
    openedIndex_ = requested;
    openedPath_ = info.filePath();
    relativePath_ = QDir(root_).relativeFilePath(openedPath_);
    if (info.isFile())
        editor_->openFile(openedPath_, previewLimit);
    else
        editor_->clearFile(tr("Select a file to open."));
    updatePath();
}

void FilesPanel::updatePath()
{
    path_->setVisible(!relativePath_.isEmpty());
    QString title = relativePath_;
    if (editor_->document()->isModified())
        title += QStringLiteral(" *");
    else if (editor_->isReadOnly() && !editor_->document()->isEmpty()) {
        if (QFileInfo(openedPath_).size() > previewLimit)
            title = tr("%1  ·  First 256 KiB").arg(title);
        else
            title = tr("%1  ·  Read only").arg(title);
    }
    path_->setText(title);
    path_->setToolTip(relativePath_);
}
