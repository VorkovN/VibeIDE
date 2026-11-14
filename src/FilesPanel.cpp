#include "FilesPanel.h"

#include <QDir>
#include <QFile>
#include <QFileSystemModel>
#include <QFontDatabase>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStringDecoder>
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
    , preview_(new QPlainTextEdit(this))
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

    preview_->setObjectName("filePreview");
    preview_->setAccessibleName(tr("File preview"));
    preview_->setReadOnly(true);
    preview_->setUndoRedoEnabled(false);
    preview_->setLineWrapMode(QPlainTextEdit::NoWrap);
    preview_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    preview_->setPlaceholderText(tr("Select a file to preview."));
    detailLayout->addWidget(preview_, 1);
    split->addWidget(details);
    split->setSizes({280, 720});
    split->setStretchFactor(1, 1);
}

void FilesPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (model_)
        return;
    if (!QFileInfo(root_).isDir()) {
        preview_->setPlaceholderText(tr("The project folder is unavailable."));
        return;
    }

    model_ = new QFileSystemModel(this);
    model_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    model_->setOption(QFileSystemModel::DontUseCustomDirectoryIcons);
    auto *files = new ProjectFiles(this);
    files->setSourceModel(model_);
    tree_->setModel(files);
    tree_->setRootIndex(files->mapFromSource(model_->setRootPath(root_)));
    for (int column = 1; column < files->columnCount(); ++column)
        tree_->hideColumn(column);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this, files](const QModelIndex &index) { showPreview(files->mapToSource(index)); });
}

void FilesPanel::showPreview(const QModelIndex &index)
{
    preview_->clear();
    const auto info = model_->fileInfo(index);
    const auto relative = QDir(root_).relativeFilePath(info.filePath());
    path_->setVisible(index.isValid());
    path_->setText(relative);
    path_->setToolTip(relative);
    if (!info.isFile()) {
        preview_->setPlaceholderText(tr("Select a file to preview."));
        return;
    }

    QFile file(info.filePath());
    if (!file.open(QIODevice::ReadOnly)) {
        preview_->setPlaceholderText(tr("Could not read this file."));
        return;
    }
    const QByteArray bytes = file.read(previewLimit + 1);
    if (file.error() != QFileDevice::NoError) {
        preview_->setPlaceholderText(tr("Could not read this file."));
        return;
    }
    const bool truncated = bytes.size() > previewLimit;
    auto decoder = QStringDecoder(QStringDecoder::Utf8);
    const auto sample = QByteArrayView(bytes).first(qMin(bytes.size(), previewLimit));
    QString text = decoder(sample);
    if (bytes.contains('\0') || decoder.hasError()) {
        preview_->setPlaceholderText(tr("Preview is available for UTF-8 text files."));
        return;
    }
    preview_->setPlaceholderText(tr("Empty file"));
    if (truncated)
        path_->setText(tr("%1  ·  First 256 KiB").arg(relative));
    preview_->setPlainText(text);
}
