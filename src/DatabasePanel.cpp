#include "DatabasePanel.h"

#include "SqliteReader.h"

#include <QAbstractTableModel>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableView>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

class DatabaseTableModel final : public QAbstractTableModel
{
public:
    explicit DatabaseTableModel(QObject *parent) : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : int(snapshot_.rows.size());
    }

    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : int(snapshot_.columns.size());
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= snapshot_.rows.size() ||
            index.column() >= snapshot_.rows.at(index.row()).size())
            return {};
        const auto &cell = snapshot_.rows.at(index.row()).at(index.column());
        if (role == Qt::DisplayRole || role == Qt::EditRole)
            return cell.text;
        if (role == Qt::ToolTipRole) {
            const auto text = cell.truncated ? cell.text + QLatin1Char('\n') + tr("Preview truncated") : cell.text;
            return QStringLiteral("<qt><pre>%1</pre></qt>").arg(text.toHtmlEscaped());
        }
        if (role == Qt::ForegroundRole && cell.kind != SqliteCell::Kind::Value)
            return QApplication::palette().color(QPalette::PlaceholderText);
        return {};
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (role != Qt::DisplayRole)
            return {};
        return orientation == Qt::Horizontal ? QVariant(snapshot_.columns.value(section))
            : QVariant(snapshot_.offset + section + 1);
    }

    void setSnapshot(const SqliteSnapshot &snapshot)
    {
        const bool sameLayout = snapshot_.path == snapshot.path && snapshot_.table == snapshot.table &&
            snapshot_.offset == snapshot.offset && snapshot_.columns == snapshot.columns &&
            snapshot_.rows.size() == snapshot.rows.size();
        if (sameLayout) {
            snapshot_ = snapshot;
            if (rowCount() > 0 && columnCount() > 0)
                emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
            return;
        }
        beginResetModel();
        snapshot_ = snapshot;
        endResetModel();
    }

    const SqliteSnapshot &snapshot() const { return snapshot_; }

private:
    SqliteSnapshot snapshot_;
};

DatabasePanel::DatabasePanel(const QString &initialDirectory, QWidget *parent)
    : QWidget(parent), initialDirectory_(initialDirectory), reader_(nullptr),
      autoRefresh_(new QCheckBox(tr("Auto-refresh"), this)), connectionPicker_(new QComboBox(this)),
      pathLabel_(new QLabel(this)),
      status_(new QLabel(this)), tables_(new QTreeWidget(this)), rows_(new QTableView(this)),
      model_(new DatabaseTableModel(this)), previous_(new QPushButton(tr("Previous"), this)),
      next_(new QPushButton(tr("Next"), this)), range_(new QLabel(this))
{
    setObjectName("databasePanel");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 9, 10, 10);
    layout->setSpacing(7);
    auto *toolbar = new QHBoxLayout;
    auto *open = new QPushButton(tr("Add Database…"), this);
    open->setObjectName("databaseOpen");
    toolbar->addWidget(open);
    connectionPicker_->setObjectName("databaseConnections");
    connectionPicker_->setAccessibleName(tr("Saved SQLite databases"));
    connectionPicker_->setPlaceholderText(tr("Saved databases"));
    connectionPicker_->setMinimumContentsLength(16);
    connectionPicker_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    connectionPicker_->setMaximumWidth(300);
    connectionPicker_->setEnabled(false);
    toolbar->addWidget(connectionPicker_);
    pathLabel_->setObjectName("databasePath");
    pathLabel_->setTextFormat(Qt::PlainText);
    pathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    toolbar->addWidget(pathLabel_, 1);
    autoRefresh_->setObjectName("databaseAutoRefresh");
    autoRefresh_->setChecked(true);
    autoRefresh_->setToolTip(tr("Update visible data every two seconds"));
    toolbar->addWidget(autoRefresh_);
    layout->addLayout(toolbar);

    auto *split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(10);
    auto *sidebar = new QWidget(split);
    auto *sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    sidebarLayout->setSpacing(5);
    auto *caption = new QLabel(tr("Tables and views"), sidebar);
    caption->setObjectName("panelCaption");
    sidebarLayout->addWidget(caption);
    tables_->setParent(sidebar);
    tables_->setObjectName("databaseTables");
    tables_->setAccessibleName(tr("Database tables and views"));
    tables_->setHeaderHidden(true);
    tables_->setRootIsDecorated(false);
    tables_->setUniformRowHeights(true);
    tables_->setTextElideMode(Qt::ElideMiddle);
    sidebarLayout->addWidget(tables_, 1);
    sidebar->setMinimumWidth(140);
    split->addWidget(sidebar);
    auto *content = new QWidget(split);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(5);
    rows_->setParent(content);
    rows_->setObjectName("databaseRows");
    rows_->setAccessibleName(tr("Database rows"));
    rows_->setToolTip(tr("Read-only SQLite data"));
    rows_->setModel(model_);
    rows_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    rows_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    rows_->setAlternatingRowColors(true);
    rows_->setWordWrap(false);
    rows_->setShowGrid(false);
    rows_->verticalHeader()->setDefaultSectionSize(28);
    rows_->horizontalHeader()->setDefaultSectionSize(160);
    rows_->horizontalHeader()->setMinimumSectionSize(48);
    rows_->horizontalHeader()->setStretchLastSection(false);
    contentLayout->addWidget(rows_, 1);
    auto *paging = new QHBoxLayout;
    previous_->setObjectName("databasePrevious");
    next_->setObjectName("databaseNext");
    range_->setObjectName("databaseRange");
    paging->addWidget(previous_);
    paging->addWidget(next_);
    paging->addWidget(range_);
    paging->addStretch(1);
    contentLayout->addLayout(paging);
    split->addWidget(content);
    split->setSizes({210, 790});
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 1);
    status_->setObjectName("databaseStatus");
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    status_->setText(tr("Open a SQLite database to browse its tables."));
    previous_->setEnabled(false);
    next_->setEnabled(false);
    updatePath();

    timer_.setInterval(2000);
    connect(&timer_, &QTimer::timeout, this, [this] {
        if (autoRefresh_->isChecked() && !busy_)
            refresh();
    });
    connect(autoRefresh_, &QCheckBox::toggled, this, [this](bool enabled) {
        emit stateChanged();
        if (enabled && isVisible())
            refresh(true);
    });
    connect(open, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("Open SQLite Database"),
            path_.isEmpty() ? initialDirectory_ : QFileInfo(path_).absolutePath(),
            tr("SQLite databases (*.db *.sqlite *.sqlite3 *.db3);;All files (*)"));
        if (path.isEmpty())
            return;
        const auto normalized = connectionPath(path);
        const auto existing = std::ranges::find(connections_, normalized, &Connection::path);
        const auto suggestedName = existing == connections_.end()
            ? QFileInfo(path).completeBaseName() : existing->name;
        QInputDialog name(this);
        name.setObjectName("databaseConnectionNameDialog");
        name.setWindowTitle(tr("Save SQLite Connection"));
        name.setLabelText(tr("Connection name"));
        name.setTextValue(suggestedName);
        if (name.exec() == QDialog::Accepted)
            openDatabase(path, name.textValue());
    });
    connect(connectionPicker_, &QComboBox::currentIndexChanged, this, &DatabasePanel::activateConnection);
    connect(tables_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (!item)
            return;
        const auto selected = item->data(0, Qt::UserRole).toString();
        if (selected == table_)
            return;
        table_ = selected;
        offset_ = 0;
        previousOffsets_.clear();
        hasMore_ = false;
        nextOffset_ = 0;
        previous_->setEnabled(false);
        next_->setEnabled(false);
        ++generation_;
        model_->setSnapshot({});
        range_->clear();
        emit stateChanged();
        refresh(true);
    });
    connect(previous_, &QPushButton::clicked, this, [this] {
        const auto previous = previousOffsets_.isEmpty() ? qMax<qint64>(0, offset_ - SqliteSnapshot::pageSize)
            : previousOffsets_.takeLast();
        changePage(previous);
    });
    connect(next_, &QPushButton::clicked, this, [this] {
        previousOffsets_.append(offset_);
        changePage(nextOffset_);
    });
    auto *copy = new QShortcut(QKeySequence::Copy, rows_);
    copy->setContext(Qt::WidgetShortcut);
    connect(copy, &QShortcut::activated, this, &DatabasePanel::copyCells);
}

DatabasePanel::~DatabasePanel()
{
    timer_.stop();
    workerThread_.requestInterruption();
    workerThread_.quit();
    workerThread_.wait();
}

QString DatabasePanel::connectionPath(const QString &path) const
{
    const auto absolute = QDir::isRelativePath(path) ? QDir(initialDirectory_).absoluteFilePath(path) : path;
    const auto canonical = QFileInfo(absolute).canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(absolute) : canonical;
}

void DatabasePanel::openDatabase(const QString &path, const QString &name)
{
    if (path.isEmpty())
        return;
    const auto normalized = connectionPath(path);
    auto existing = std::ranges::find(connections_, normalized, &Connection::path);
    if (existing == connections_.end()) {
        const auto displayName = name.trimmed().isEmpty() ? QFileInfo(normalized).completeBaseName() : name.trimmed();
        connections_.append({displayName.isEmpty() ? QFileInfo(normalized).fileName() : displayName, normalized, {}});
        existing = connections_.end() - 1;
    } else if (!name.trimmed().isEmpty()) {
        existing->name = name.trimmed();
    }
    const int index = int(existing - connections_.begin());
    updateConnections();
    activateConnection(index);
}

void DatabasePanel::rememberCurrentTable()
{
    const auto current = std::ranges::find(connections_, path_, &Connection::path);
    if (current != connections_.end())
        current->table = table_;
}

void DatabasePanel::updateConnections()
{
    const QSignalBlocker blocked(connectionPicker_);
    connectionPicker_->clear();
    int active = -1;
    for (int index = 0; index < connections_.size(); ++index) {
        const auto &connection = connections_.at(index);
        connectionPicker_->addItem(connection.name, connection.path);
        connectionPicker_->setItemData(index, connection.path, Qt::ToolTipRole);
        if (connection.path == path_)
            active = index;
    }
    connectionPicker_->setCurrentIndex(active);
    connectionPicker_->setEnabled(!connections_.isEmpty());
}

void DatabasePanel::activateConnection(int index)
{
    if (index < 0 || index >= connections_.size())
        return;
    rememberCurrentTable();
    const auto connection = connections_.at(index);
    if (connection.path != path_) {
        path_ = connection.path;
        table_ = connection.table;
        offset_ = 0;
        previousOffsets_.clear();
        hasMore_ = false;
        nextOffset_ = 0;
        {
            const QSignalBlocker blocked(tables_);
            tables_->clear();
        }
        model_->setSnapshot({});
        range_->clear();
        previous_->setEnabled(false);
        next_->setEnabled(false);
    }
    ++generation_;
    {
        const QSignalBlocker blocked(connectionPicker_);
        connectionPicker_->setCurrentIndex(index);
    }
    updatePath();
    emit stateChanged();
    status_->setText(tr("Loading database…"));
    status_->show();
    refresh(true);
}

void DatabasePanel::restoreState(const QJsonObject &state)
{
    ++generation_;
    connections_.clear();
    path_.clear();
    table_.clear();
    for (const auto &item : state.value(QStringLiteral("connections")).toArray()) {
        const auto saved = item.toObject();
        const auto savedPath = saved.value(QStringLiteral("path")).toString();
        if (savedPath.isEmpty())
            continue;
        const auto path = connectionPath(savedPath);
        if (std::ranges::find(connections_, path, &Connection::path) != connections_.end())
            continue;
        auto name = saved.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty())
            name = QFileInfo(path).completeBaseName();
        connections_.append({name.isEmpty() ? QFileInfo(path).fileName() : name, path,
                             saved.value(QStringLiteral("table")).toString()});
    }
    const auto savedPath = state.value(QStringLiteral("path")).toString();
    if (!savedPath.isEmpty()) {
        path_ = connectionPath(savedPath);
        const auto active = std::ranges::find(connections_, path_, &Connection::path);
        if (active == connections_.end()) {
            const QFileInfo info(path_);
            connections_.append({info.completeBaseName().isEmpty() ? info.fileName() : info.completeBaseName(),
                                 path_, state.value(QStringLiteral("table")).toString()});
            table_ = connections_.constLast().table;
        } else {
            table_ = active->table;
        }
    } else if (!connections_.isEmpty()) {
        path_ = connections_.constFirst().path;
        table_ = connections_.constFirst().table;
    }
    offset_ = 0;
    previousOffsets_.clear();
    hasMore_ = false;
    nextOffset_ = 0;
    previous_->setEnabled(false);
    next_->setEnabled(false);
    {
        const QSignalBlocker blocked(tables_);
        tables_->clear();
    }
    model_->setSnapshot({});
    range_->clear();
    const QSignalBlocker blocked(autoRefresh_);
    autoRefresh_->setChecked(state.value(QStringLiteral("autoRefresh")).toBool(true));
    updateConnections();
    updatePath();
    if (!path_.isEmpty()) {
        status_->setText(tr("Loading database…"));
        refresh(true);
    }
}

QJsonObject DatabasePanel::saveState() const
{
    QJsonArray connections;
    for (const auto &connection : connections_) {
        connections.append(QJsonObject{{QStringLiteral("name"), connection.name},
            {QStringLiteral("path"), connection.path},
            {QStringLiteral("table"), connection.path == path_ ? table_ : connection.table}});
    }
    return {{QStringLiteral("path"), path_}, {QStringLiteral("table"), table_},
            {QStringLiteral("autoRefresh"), autoRefresh_->isChecked()},
            {QStringLiteral("connections"), connections}};
}

void DatabasePanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    timer_.start();
    refresh(true);
}

void DatabasePanel::hideEvent(QHideEvent *event)
{
    timer_.stop();
    QWidget::hideEvent(event);
}

void DatabasePanel::refresh(bool force)
{
    if (path_.isEmpty() || !isVisible())
        return;
    if (busy_) {
        pendingRefresh_ = true;
        pendingForce_ |= force;
        return;
    }
    startReader();
    busy_ = true;
    const auto generation = ++generation_;
    const auto path = path_;
    const auto table = table_;
    const auto offset = offset_;
    QMetaObject::invokeMethod(reader_, [reader = reader_, path, table, offset, generation, force] {
        reader->request(path, table, offset, generation, force);
    }, Qt::QueuedConnection);
}

void DatabasePanel::startReader()
{
    if (reader_)
        return;
    reader_ = new SqliteReader;
    reader_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, reader_, &QObject::deleteLater);
    connect(reader_, &SqliteReader::ready, this, [this](const SqliteSnapshot &snapshot) {
        busy_ = false;
        if (snapshot.generation == generation_)
            showSnapshot(snapshot);
        finishRequest();
    });
    connect(reader_, &SqliteReader::failed, this, [this](quint64 generation, const QString &error) {
        busy_ = false;
        if (generation == generation_) {
            status_->setText(error);
            status_->show();
            previous_->setEnabled(offset_ > 0);
            next_->setEnabled(hasMore_);
        }
        finishRequest();
    });
    workerThread_.start();
}

void DatabasePanel::finishRequest()
{
    if (pendingRefresh_) {
        pendingRefresh_ = false;
        const bool force = std::exchange(pendingForce_, false);
        refresh(force);
    }
}

void DatabasePanel::showSnapshot(const SqliteSnapshot &snapshot)
{
    const auto &previous = model_->snapshot();
    const bool samePage = previous.path == snapshot.path && previous.table == snapshot.table &&
                          previous.offset == snapshot.offset && previous.columns == snapshot.columns;
    const int horizontal = rows_->horizontalScrollBar()->value();
    const int vertical = rows_->verticalScrollBar()->value();
    const auto current = rows_->currentIndex();
    const auto selected = rows_->selectionModel()->selectedIndexes();
    const bool tableChanged = table_ != snapshot.table;
    if (tableChanged || offset_ != snapshot.offset)
        previousOffsets_.clear();
    table_ = snapshot.table;
    rememberCurrentTable();
    offset_ = snapshot.offset;
    hasMore_ = snapshot.hasMore;
    nextOffset_ = offset_ + snapshot.rows.size();
    {
        const QSignalBlocker blocked(tables_);
        bool sameTables = tables_->topLevelItemCount() == snapshot.tables.size();
        for (int index = 0; sameTables && index < snapshot.tables.size(); ++index) {
            const auto *item = tables_->topLevelItem(index);
            sameTables = item->data(0, Qt::UserRole).toString() == snapshot.tables.at(index).name &&
                         item->data(0, Qt::UserRole + 1).toBool() == snapshot.tables.at(index).view;
        }
        if (!sameTables) {
            const int scroll = tables_->verticalScrollBar()->value();
            tables_->clear();
            for (const auto &table : snapshot.tables) {
                auto *item = new QTreeWidgetItem(tables_, {table.name});
                item->setData(0, Qt::UserRole, table.name);
                item->setData(0, Qt::UserRole + 1, table.view);
                item->setToolTip(0, table.view ? tr("View") : tr("Table"));
            }
            tables_->verticalScrollBar()->setValue(scroll);
        }
        for (int index = 0; index < tables_->topLevelItemCount(); ++index) {
            auto *item = tables_->topLevelItem(index);
            if (item->data(0, Qt::UserRole).toString() == table_)
                tables_->setCurrentItem(item);
        }
    }
    if (snapshot.changed || !samePage) {
        model_->setSnapshot(snapshot);
        if (samePage) {
            if (current.isValid())
                rows_->setCurrentIndex(model_->index(current.row(), current.column()));
            QItemSelection selection;
            for (const auto &index : selected) {
                const auto cell = model_->index(index.row(), index.column());
                if (cell.isValid())
                    selection.select(cell, cell);
            }
            rows_->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
            rows_->horizontalScrollBar()->setValue(horizontal);
            rows_->verticalScrollBar()->setValue(vertical);
        }
    }
    previous_->setEnabled(offset_ > 0);
    next_->setEnabled(hasMore_);
    range_->setText(snapshot.rows.isEmpty() ? tr("No rows") : tr("Rows %1–%2").arg(offset_ + 1).arg(offset_ + snapshot.rows.size()));
    QString status = snapshot.error;
    if (status.isEmpty() && snapshot.tables.isEmpty())
        status = tr("This database has no tables or views.");
    status_->setText(status);
    status_->setVisible(!status_->text().isEmpty());
    if (tableChanged)
        emit stateChanged();
}

void DatabasePanel::updatePath()
{
    pathLabel_->setText(path_.isEmpty() ? tr("No database selected") : QFileInfo(path_).fileName());
    pathLabel_->setToolTip(path_);
}

void DatabasePanel::changePage(qint64 offset)
{
    offset_ = offset;
    hasMore_ = false;
    ++generation_;
    previous_->setEnabled(false);
    next_->setEnabled(false);
    refresh(true);
}

void DatabasePanel::copyCells()
{
    auto cells = rows_->selectionModel()->selectedIndexes();
    if (cells.isEmpty())
        return;
    std::ranges::sort(cells, [](const QModelIndex &first, const QModelIndex &second) {
        return first.row() == second.row() ? first.column() < second.column() : first.row() < second.row();
    });
    QStringList lines;
    QStringList values;
    int previousRow = cells.first().row();
    for (const auto &cell : cells) {
        if (cell.row() != previousRow) {
            lines.append(values.join(u'\t'));
            values.clear();
        }
        values.append(cell.data().toString());
        previousRow = cell.row();
    }
    lines.append(values.join(u'\t'));
    QApplication::clipboard()->setText(lines.join(u'\n'));
}
