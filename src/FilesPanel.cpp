#include "FilesPanel.h"

#include "FileTransfer.h"
#include "FindBar.h"
#include "TextFileEditor.h"

#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QGuiApplication>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QScopedValueRollback>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStyleOptionViewItem>
#include <QTextDocument>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

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

class ProjectFileTree final : public QTreeView
{
public:
    ProjectFileTree(const QString &root, FilesPanel *panel)
        : QTreeView(panel), root_(QFileInfo(root).canonicalFilePath()), panel_(panel)
    {
        setSelectionMode(ExtendedSelection);
        setSelectionBehavior(SelectRows);
        setDragDropMode(DragDrop);
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->matches(QKeySequence::Copy)) {
            copySelection();
        } else if (event->matches(QKeySequence::Paste)) {
            pasteInto(destination(selectedIndex()));
        } else {
            QTreeView::keyPressEvent(event);
            return;
        }
        event->accept();
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        const auto target = event->reason() == QContextMenuEvent::Keyboard ? selectedIndex() : indexAt(event->pos());
        const auto folder = destination(target);
        if (target.isValid() && !selectionModel()->isSelected(target))
            setCurrentIndex(target);

        QMenu menu(this);
        auto *copy = menu.addAction(FilesPanel::tr("Copy"));
        auto *paste = menu.addAction(FilesPanel::tr("Paste"));
        copy->setEnabled(!selectionPaths().isEmpty());
        paste->setEnabled(!folder.isEmpty() &&
                          !localPaths(QGuiApplication::clipboard()->mimeData()).isEmpty());
        const auto *chosen = menu.exec(event->globalPos());
        if (chosen == copy)
            copySelection();
        else if (chosen == paste)
            pasteInto(folder);
    }

    void startDrag(Qt::DropActions) override
    {
        const auto paths = selectionPaths();
        if (paths.isEmpty() || !prepareTransfer())
            return;
        auto *drag = new QDrag(this);
        drag->setMimeData(fileMimeData(paths));
        drag->exec(Qt::CopyAction | Qt::MoveAction, Qt::CopyAction);
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        acceptFileDrag(event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        acceptFileDrag(event);
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        setDropTarget({});
        event->accept();
    }

    void dropEvent(QDropEvent *event) override
    {
        setDropTarget({});
        const auto action = dropAction(*event);
        if (!event->possibleActions().testFlag(action) ||
            !transfer(localPaths(event->mimeData()), destination(indexAt(event->position().toPoint())), action)) {
            event->ignore();
            return;
        }
        event->setDropAction(action);
        event->accept();
    }

    void drawRow(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto row = option;
        if (index == dropTarget_)
            row.state |= QStyle::State_MouseOver;
        QTreeView::drawRow(painter, row, index);
    }

private:
    QModelIndex selectedIndex() const
    {
        const auto index = currentIndex();
        return index.isValid() && selectionModel()->isSelected(index) ? index : QModelIndex{};
    }

    QStringList selectionPaths() const
    {
        QStringList paths;
        if (selectionModel()) {
            for (const auto &index : selectionModel()->selectedRows())
                paths.append(index.data(QFileSystemModel::FilePathRole).toString());
        }
        return FileTransfer::topLevelPaths(paths);
    }

    static QStringList localPaths(const QMimeData *data)
    {
        QStringList paths;
        if (data && data->hasUrls()) {
            for (const auto &url : data->urls()) {
                if (url.isLocalFile() && QDir::isAbsolutePath(url.toLocalFile()))
                    paths.append(url.toLocalFile());
            }
        }
        return paths;
    }

    static QMimeData *fileMimeData(const QStringList &paths)
    {
        QList<QUrl> urls;
        for (const auto &path : paths)
            urls.append(QUrl::fromLocalFile(path));
        auto *data = new QMimeData;
        data->setUrls(urls);
        return data;
    }

    QString destination(const QModelIndex &index) const
    {
        const QFileInfo item(index.isValid() ? index.data(QFileSystemModel::FilePathRole).toString() : root_);
        const QFileInfo folder(item.isDir() ? item : QFileInfo(item.absolutePath()));
        const auto path = folder.canonicalFilePath();
        return folder.isDir() && insideProject(path) ? path : QString{};
    }

    bool insideProject(const QString &path) const
    {
        if (path.isEmpty() || root_.isEmpty())
            return false;
        const auto relative = QDir(root_).relativeFilePath(path);
        return relative != QStringLiteral("..") && !relative.startsWith(QStringLiteral("../")) &&
               !QDir::isAbsolutePath(relative) && !relative.split(u'/').contains(QStringLiteral(".git"));
    }

    Qt::DropAction dropAction(const QDropEvent &event) const
    {
        if (event.source() == this && !event.modifiers().testFlag(Qt::AltModifier))
            return Qt::MoveAction;
        return Qt::CopyAction;
    }

    void acceptFileDrag(QDropEvent *event)
    {
        const auto index = indexAt(event->position().toPoint());
        const auto action = dropAction(*event);
        if (!event->possibleActions().testFlag(action) || destination(index).isEmpty() ||
            localPaths(event->mimeData()).isEmpty()) {
            setDropTarget({});
            event->ignore();
            return;
        }
        const QFileInfo item(index.data(QFileSystemModel::FilePathRole).toString());
        setDropTarget(item.isDir() ? index : index.parent());
        event->setDropAction(action);
        event->accept();
    }

    void setDropTarget(const QModelIndex &index)
    {
        if (dropTarget_ != index) {
            dropTarget_ = index;
            viewport()->update();
        }
    }

    void copySelection()
    {
        const auto paths = selectionPaths();
        if (!paths.isEmpty() && prepareTransfer())
            QGuiApplication::clipboard()->setMimeData(fileMimeData(paths));
    }

    void pasteInto(const QString &folder)
    {
        transfer(localPaths(QGuiApplication::clipboard()->mimeData()), folder, Qt::CopyAction);
    }

    bool prepareTransfer()
    {
        if (panel_->flushAutosave())
            return true;
        QMessageBox::warning(this, FilesPanel::tr("File transfer"), panel_->autosaveError());
        return false;
    }

    bool transfer(const QStringList &paths, const QString &folder, Qt::DropAction action)
    {
        if (paths.isEmpty() || folder.isEmpty())
            return false;
        if (action == Qt::MoveAction && !std::ranges::all_of(paths, [this](const QString &path) {
                return insideProject(QFileInfo(path).dir().canonicalPath());
            })) {
            QMessageBox::warning(this, FilesPanel::tr("File transfer"),
                                 FilesPanel::tr("A source file is outside the project folder."));
            return false;
        }
        if (!prepareTransfer())
            return false;
        const auto result = FileTransfer::transfer(paths, folder, action);
        panel_->reloadIfClean();
        if (!result.errors.isEmpty())
            QMessageBox::warning(this, FilesPanel::tr("File transfer"), result.errors.join(u'\n'));
        return result.errors.isEmpty();
    }

    const QString root_;
    FilesPanel *const panel_;
    QPersistentModelIndex dropTarget_;
};
}

FilesPanel::FilesPanel(const QString &root, QWidget *parent)
    : QWidget(parent)
    , root_(root)
    , path_(new QLabel(this))
    , tree_(new ProjectFileTree(root, this))
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
    auto *find = new FindBar(this);
    detailLayout->addWidget(find);
    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    findShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(findShortcut, &QShortcut::activated, find, [this, find] { find->openFor(editor_); });

    editor_->setObjectName("fileEditor");
    editor_->setAccessibleName(tr("File editor"));
    editor_->clearFile(tr("Select a file to open."));
    connect(editor_->document(), &QTextDocument::modificationChanged, this, &FilesPanel::updatePath);
    connect(editor_, &TextFileEditor::autosaveErrorChanged, this, &FilesPanel::updatePath);
    detailLayout->addWidget(editor_, 1);
    split->addWidget(details);
    split->setSizes({280, 720});
    split->setStretchFactor(1, 1);
}

bool FilesPanel::flushAutosave()
{
    return editor_->flushAutosave();
}

const QString &FilesPanel::autosaveError() const
{
    return editor_->autosaveError();
}

void FilesPanel::reloadIfClean()
{
    editor_->reloadIfClean();
    updatePath();
}

void FilesPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (model_) {
        reloadIfClean();
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
        reloadIfClean();
        return;
    }
    if (!flushAutosave() || !requested.isValid()) {
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
    const auto &error = autosaveError();
    if (!error.isEmpty())
        title = tr("Autosave failed  ·  %1").arg(title);
    else if (editor_->document()->isModified())
        title += QStringLiteral(" *");
    else if (editor_->isReadOnly() && !editor_->document()->isEmpty()) {
        if (QFileInfo(openedPath_).size() > previewLimit)
            title = tr("%1  ·  First 256 KiB").arg(title);
        else
            title = tr("%1  ·  Read only").arg(title);
    }
    path_->setText(title);
    path_->setToolTip(error.isEmpty() ? relativePath_ : relativePath_ + u'\n' + error);
}
