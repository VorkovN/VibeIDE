#include "CurrentChangesPanel.h"

#include "GitFileList.h"
#include "GitStatus.h"

#include <QHeaderView>
#include <QLabel>
#include <QProcessEnvironment>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr qsizetype outputLimit = 2 * 1024 * 1024;
constexpr qsizetype errorLimit = 8 * 1024;
}

CurrentChangesPanel::CurrentChangesPanel(const QString &root, QWidget *parent)
    : QWidget(parent)
    , files_(new QTreeWidget(this))
    , status_(new QLabel(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    files_->setObjectName("currentChanges");
    files_->setAccessibleName(tr("Files with uncommitted changes"));
    files_->setHeaderLabels({tr("File")});
    files_->setRootIsDecorated(false);
    files_->setUniformRowHeights(true);
    files_->setTextElideMode(Qt::ElideMiddle);
    files_->header()->setStretchLastSection(false);
    files_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    GitFileList::useStatusColors(*files_);
    connect(files_, &QTreeWidget::itemClicked, this, [this](const QTreeWidgetItem *item) {
        emit viewChangesRequested(item->data(0, Qt::UserRole).toByteArray());
    });
    layout->addWidget(files_, 1);
    status_->setObjectName("muted");
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->setAlignment(Qt::AlignCenter);
    layout->addWidget(status_, 1);
    files_->hide();

    git_.setWorkingDirectory(root);
    git_.setStandardInputFile(QProcess::nullDevice());
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto *variable : {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR"})
        environment.remove(QString::fromLatin1(variable));
    environment.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    git_.setProcessEnvironment(environment);
    connect(&git_, &QProcess::readyReadStandardOutput, this, &CurrentChangesPanel::collectOutput);
    connect(&git_, &QProcess::readyReadStandardError, this, &CurrentChangesPanel::collectOutput);
    connect(&git_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            showError(tr("Could not start Git. %1").arg(git_.errorString()));
    });
    connect(&git_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus exitStatus) {
        collectOutput();
        if (oversized_) {
            showError(tr("There are too many changed files to display."));
        } else if (code != 0 || exitStatus != QProcess::NormalExit) {
            const auto detail = QString::fromUtf8(errors_).trimmed();
            showError(detail.isEmpty() ? tr("Could not read Git status.") : detail);
        } else {
            showChanges();
        }
    });
    timer_.setInterval(5000);
    connect(&timer_, &QTimer::timeout, this, &CurrentChangesPanel::refresh);
}

CurrentChangesPanel::~CurrentChangesPanel()
{
    disconnect(&git_, nullptr, this, nullptr);
    if (git_.state() != QProcess::NotRunning) {
        git_.kill();
        git_.waitForFinished(300);
    }
}

void CurrentChangesPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refresh();
    timer_.start();
}

void CurrentChangesPanel::hideEvent(QHideEvent *event)
{
    timer_.stop();
    QWidget::hideEvent(event);
}

void CurrentChangesPanel::refresh()
{
    if (git_.state() != QProcess::NotRunning)
        return;
    if (!loaded_)
        status_->setText(tr("Loading current changes…"));
    output_.clear();
    errors_.clear();
    oversized_ = false;
    git_.start(QStringLiteral("git"), GitStatus::arguments());
}

void CurrentChangesPanel::collectOutput()
{
    const auto output = git_.readAllStandardOutput();
    const auto errors = git_.readAllStandardError();
    errors_.append(errors.constData(), qMin(errors.size(), errorLimit - errors_.size()));
    if (oversized_)
        return;
    if (output.size() > outputLimit - output_.size()) {
        oversized_ = true;
        git_.kill();
        return;
    }
    output_.append(output);
}

void CurrentChangesPanel::showChanges()
{
    if (loaded_ && displayed_ == output_)
        return;
    displayed_ = std::move(output_);
    loaded_ = true;

    const auto *selected = files_->currentItem();
    const auto selectedPath = selected ? selected->data(0, Qt::UserRole).toByteArray() : QByteArray{};
    files_->clear();
    const auto changes = GitStatus::parse(displayed_);
    for (const auto &change : changes) {
        auto path = GitStatus::displayPath(change.path);
        if (!change.previousPath.isEmpty())
            path = GitStatus::displayPath(change.previousPath) + QStringLiteral(" → ") + path;
        auto *item = new QTreeWidgetItem(files_, {path});
        GitFileList::setStatus(*item, *files_, change.status);
        item->setData(0, Qt::UserRole, change.path);
        if (change.path == selectedPath)
            files_->setCurrentItem(item);
    }
    files_->setVisible(!changes.isEmpty());
    status_->setVisible(changes.isEmpty());
    status_->setText(tr("No uncommitted changes."));
}

void CurrentChangesPanel::showError(const QString &message)
{
    loaded_ = false;
    files_->clear();
    files_->hide();
    status_->setText(message);
    status_->show();
}
