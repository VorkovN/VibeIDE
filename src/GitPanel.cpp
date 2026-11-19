#include "GitPanel.h"

#include "GitActions.h"
#include "GitSquash.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFile>
#include <QFontDatabase>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <utility>

namespace {

constexpr int pageSize = 250;
constexpr qsizetype historyLimit = 2 * 1024 * 1024;
constexpr qsizetype errorLimit = 16 * 1024;
constexpr int refsColumn = 1;
constexpr int subjectColumn = 2;
constexpr int authorColumn = 3;
constexpr int dateColumn = 4;
constexpr int changesColumn = 5;
constexpr int filesColumn = 6;
constexpr int hashColumn = 7;
constexpr int changeCountsRole = Qt::UserRole + 2;
constexpr int laneSpacing = 14;
constexpr int graphMargin = 12;
constexpr int laneColorCount = 6;

enum class RefKind { Current, Branch, Tag, Remote, Other };

struct RefLabel {
    QString name;
    RefKind kind;
};

constexpr int refChipGap = 5;
constexpr int refChipPadding = 20;

QFont refFont(QFont font)
{
    font.setPointSizeF(qMax(8.0, font.pointSizeF() - 1.0));
    font.setWeight(QFont::Medium);
    return font;
}

int refsWidth(const QList<RefLabel> &refs, const QFontMetrics &metrics)
{
    int width = 15;
    for (qsizetype index = 0; index < refs.size(); ++index)
        width += (index > 0 ? refChipGap : 0) + metrics.horizontalAdvance(refs.at(index).name) + refChipPadding;
    return width;
}

struct GraphEdge {
    int lane;
    int color;
};

} // namespace

struct GitPanel::HistoryItem final : public QTreeWidgetItem
{
    using QTreeWidgetItem::QTreeWidgetItem;

    QList<RefLabel> refs;
    QList<GraphEdge> continuing;
    QList<GraphEdge> parents;
    GraphEdge node{};
    bool incoming = false;
};

namespace {

QList<RefLabel> refLabels(const QByteArray &decoration)
{
    const QString currentBranchPrefix = QStringLiteral("HEAD -> refs/heads/");
    const QString branchPrefix = QStringLiteral("refs/heads/");
    const QString tagPrefix = QStringLiteral("tag: refs/tags/");
    const QString remotePrefix = QStringLiteral("refs/remotes/");
    QList<RefLabel> labels;
    const auto refs = QString::fromUtf8(decoration).split(QStringLiteral(", "), Qt::SkipEmptyParts);
    for (const auto &ref : refs) {
        if (ref.startsWith(currentBranchPrefix)) {
            labels.append({QStringLiteral("HEAD → ") + ref.mid(currentBranchPrefix.size()), RefKind::Current});
        } else if (ref == QStringLiteral("HEAD")) {
            labels.append({ref, RefKind::Current});
        } else if (ref.startsWith(branchPrefix)) {
            labels.append({ref.mid(branchPrefix.size()), RefKind::Branch});
        } else if (ref.startsWith(tagPrefix)) {
            labels.append({ref.mid(tagPrefix.size()), RefKind::Tag});
        } else if (ref.startsWith(remotePrefix)) {
            labels.append({ref.mid(remotePrefix.size()), RefKind::Remote});
        } else {
            labels.append({ref.startsWith(QStringLiteral("refs/")) ? ref.mid(5) : ref, RefKind::Other});
        }
    }
    return labels;
}

} // namespace

int GitPanel::laneFor(const QByteArray &commit) const
{
    for (int lane = 0; lane < lanes_.size(); ++lane) {
        if (lanes_[lane].commit == commit)
            return lane;
    }
    return -1;
}

int GitPanel::openLane(int from)
{
    for (int lane = from; lane < lanes_.size(); ++lane) {
        if (lanes_[lane].commit.isEmpty())
            return lane;
    }
    lanes_.append(GraphLane{});
    return int(lanes_.size() - 1);
}

void GitPanel::setGraphRow(HistoryItem &item, const QByteArray &hash, const QByteArray &parentHashes)
{
    int position = laneFor(hash);
    item.incoming = position >= 0;
    if (!item.incoming) {
        position = openLane();
        lanes_[position] = {hash, nextLaneColor_++ % laneColorCount};
    }
    item.node = {position, lanes_[position].color};
    for (int lane = 0; lane < lanes_.size(); ++lane) {
        if (lane != position && !lanes_[lane].commit.isEmpty())
            item.continuing.append({lane, lanes_[lane].color});
    }

    lanes_[position].commit.clear();
    if (!parentHashes.isEmpty()) {
        const auto parents = parentHashes.split(' ');
        for (qsizetype parent = 0; parent < parents.size(); ++parent) {
            int lane = laneFor(parents[parent]);
            if (lane < 0) {
                lane = parent == 0 ? position : openLane(position + 1);
                const int color = parent == 0 ? item.node.color : nextLaneColor_++ % laneColorCount;
                lanes_[lane] = {parents[parent], color};
            }
            item.parents.append({lane, lanes_[lane].color});
        }
    }
    while (!lanes_.isEmpty() && lanes_.last().commit.isEmpty())
        lanes_.removeLast();
}

namespace {

QColor laneColor(int color, bool dark)
{
    static const std::array<QColor, laneColorCount> darkColors = {
        QColor("#80b2f8"), QColor("#75e2b5"), QColor("#bfacf4"),
        QColor("#e8bd80"), QColor("#e999bf"), QColor("#72cfd0")};
    static const std::array<QColor, laneColorCount> lightColors = {
        QColor("#3266ae"), QColor("#167352"), QColor("#7256ac"),
        QColor("#926223"), QColor("#aa496f"), QColor("#16747d")};
    return (dark ? darkColors : lightColors)[color];
}

QColor refColor(RefKind kind, bool dark)
{
    switch (kind) {
    case RefKind::Current: return laneColor(1, dark);
    case RefKind::Branch: return laneColor(0, dark);
    case RefKind::Tag: return laneColor(3, dark);
    case RefKind::Remote: return laneColor(2, dark);
    case RefKind::Other: return laneColor(5, dark);
    }
    return laneColor(0, dark);
}

} // namespace

class GitPanel::HistoryDelegate final : public QStyledItemDelegate
{
public:
    explicit HistoryDelegate(QTreeWidget *tree) : QStyledItemDelegate(tree), tree_(tree) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(size.height(), 31));
        if (index.column() == refsColumn) {
            const auto *item = static_cast<const HistoryItem *>(tree_->itemFromIndex(index));
            size.setWidth(refsWidth(item->refs, QFontMetrics(refFont(option.font))));
        }
        if (index.column() == changesColumn)
            size.setWidth(qMax(size.width(), option.fontMetrics.horizontalAdvance(index.data().toString()) + 12));
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const auto *item = static_cast<const HistoryItem *>(tree_->itemFromIndex(index));
        if (!item)
            return;
        QStyleOptionViewItem row = option;
        initStyleOption(&row, index);
        const auto text = std::exchange(row.text, {});
        tree_->style()->drawControl(QStyle::CE_ItemViewItem, &row, painter, tree_);

        painter->save();
        painter->setClipRect(row.rect, Qt::IntersectClip);
        const bool selected = row.state.testFlag(QStyle::State_Selected);
        const auto base = row.features.testFlag(QStyleOptionViewItem::Alternate)
            ? QPalette::AlternateBase : QPalette::Base;
        const auto background = row.palette.color(selected ? QPalette::Highlight : base);
        const bool dark = background.lightness() < 128;
        if (index.column() == 0)
            paintGraph(*painter, row, *item, background, dark);
        else if (index.column() == refsColumn)
            paintRefs(*painter, row, *item, dark);
        else if (index.column() == subjectColumn)
            paintSummary(*painter, row, *item);
        else if (index.column() == changesColumn)
            paintChanges(*painter, row, index.data(changeCountsRole).toStringList(), dark);
        else
            paintMetadata(*painter, row, text);
        painter->restore();
    }

private:
    static void paintMetadata(QPainter &painter, const QStyleOptionViewItem &row, const QString &text)
    {
        const bool selected = row.state.testFlag(QStyle::State_Selected);
        const auto content = row.rect.adjusted(7, 0, -7, 0);
        painter.setFont(row.font);
        painter.setPen(row.palette.color(selected ? QPalette::HighlightedText : QPalette::PlaceholderText));
        painter.drawText(content, row.displayAlignment | Qt::AlignVCenter,
                         row.fontMetrics.elidedText(text, Qt::ElideRight, content.width()));
    }

    static void paintChanges(QPainter &painter, const QStyleOptionViewItem &row,
                             const QStringList &counts, bool dark)
    {
        const bool selected = row.state.testFlag(QStyle::State_Selected);
        const auto text = row.palette.color(selected ? QPalette::HighlightedText : QPalette::PlaceholderText);
        QRect content = row.rect.adjusted(7, 0, -5, 0);
        painter.setFont(row.font);
        if (counts.isEmpty()) {
            painter.setPen(text);
            painter.drawText(content, Qt::AlignVCenter, QStringLiteral("—"));
            return;
        }

        const std::array colors{QColor(dark ? "#86e3ad" : "#167342"),
                                QColor(dark ? "#ffa2ae" : "#a53549")};
        for (qsizetype i = 0; i < qMin<qsizetype>(counts.size(), colors.size()); ++i) {
            const auto &count = counts.at(i);
            painter.setPen(count.size() == 2 && count.endsWith(u'0') ? text : colors[i]);
            painter.drawText(content, Qt::AlignVCenter, count);
            content.setLeft(content.left() + row.fontMetrics.horizontalAdvance(count + QStringLiteral("  ")));
        }
    }

    static void paintGraph(QPainter &painter, const QStyleOptionViewItem &row,
                           const HistoryItem &item, const QColor &background, bool dark)
    {
        painter.setRenderHint(QPainter::Antialiasing);
        const auto x = [&row](int lane) { return row.rect.left() + graphMargin + lane * laneSpacing; };
        const int top = row.rect.top();
        const int middle = top + row.rect.height() / 2;
        const int bottom = top + row.rect.height();
        const int node = x(item.node.lane);
        const QColor nodeColor = laneColor(item.node.color, dark);
        for (const auto &edge : item.continuing) {
            painter.setPen(QPen(laneColor(edge.color, dark), 1.5, Qt::SolidLine, Qt::FlatCap));
            painter.drawLine(x(edge.lane), top, x(edge.lane), bottom);
        }
        if (item.incoming) {
            painter.setPen(QPen(nodeColor, 1.5, Qt::SolidLine, Qt::FlatCap));
            painter.drawLine(node, top, node, middle);
        }
        for (const auto &edge : item.parents) {
            painter.setPen(QPen(laneColor(edge.color, dark), 1.5, Qt::SolidLine, Qt::FlatCap));
            if (edge.lane == item.node.lane) {
                painter.drawLine(node, middle, node, bottom);
            } else {
                const int target = x(edge.lane);
                QPainterPath connection(QPointF(node, middle));
                const qreal control = node + (target - node) * 0.68;
                connection.cubicTo(control, middle, target, (middle + bottom) / 2.0, target, bottom);
                painter.drawPath(connection);
            }
        }
        painter.setPen(QPen(background, 1.3));
        painter.setBrush(nodeColor);
        const bool merge = item.parents.size() > 1;
        const qreal radius = merge ? 5.0 : 4.0;
        painter.drawEllipse(QPointF(node, middle), radius, radius);
        if (merge) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(background);
            painter.drawEllipse(QPointF(node, middle), 2.0, 2.0);
        }
    }

    static void paintSummary(QPainter &painter, const QStyleOptionViewItem &row,
                             const HistoryItem &item)
    {
        const QRect content = row.rect.adjusted(7, 0, -8, 0);
        const bool selected = row.state.testFlag(QStyle::State_Selected);
        const auto text = row.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        const bool head = std::ranges::any_of(item.refs, [](const RefLabel &ref) {
            return ref.kind == RefKind::Current;
        });
        QFont subjectFont = row.font;
        if (head)
            subjectFont.setWeight(QFont::DemiBold);
        painter.setFont(subjectFont);
        painter.setPen(text);
        painter.drawText(content, Qt::AlignVCenter,
                         QFontMetrics(subjectFont).elidedText(item.text(subjectColumn), Qt::ElideRight, content.width()));
    }

    static void paintRefs(QPainter &painter, const QStyleOptionViewItem &row,
                          const HistoryItem &item, bool dark)
    {
        const QRect content = row.rect.adjusted(7, 0, -8, 0);
        const bool selected = row.state.testFlag(QStyle::State_Selected);
        const auto chipFont = refFont(row.font);
        const QFontMetrics chipMetrics(chipFont);
        const int chipHeight = qMin(19, content.height() - 4);
        const int top = content.top() + (content.height() - chipHeight) / 2;
        int left = content.left();
        painter.setFont(chipFont);
        painter.setRenderHint(QPainter::Antialiasing);
        for (const auto &ref : item.refs) {
            const int width = chipMetrics.horizontalAdvance(ref.name) + refChipPadding;
            const QRect chip(left, top, width, chipHeight);
            const QColor color = refColor(ref.kind, dark);
            QColor fill = color;
            const int normalAlpha = ref.kind == RefKind::Current ? 46 : 26;
            fill.setAlpha(selected ? 20 : normalAlpha);
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawRoundedRect(chip, 6, 6);
            painter.setBrush(color);
            painter.drawEllipse(QPointF(chip.left() + 8, chip.center().y() + 0.5), 2.0, 2.0);
            painter.setPen(dark ? color : color.darker(112));
            painter.drawText(chip.adjusted(14, 0, -6, 0), Qt::AlignVCenter,
                             ref.name);
            left = chip.right() + 1 + refChipGap;
        }
    }

    QTreeWidget *tree_;
};

namespace {

class GitCommand final : public QProcess
{
public:
    using QProcess::QProcess;

    QByteArray output;
    QByteArray error;
    bool truncated = false;
};

void collect(QByteArray &destination, const QByteArray &chunk, qsizetype limit)
{
    const qsizetype remaining = limit - destination.size();
    if (remaining > 0)
        destination.append(chunk.constData(), qMin(chunk.size(), remaining));
}

} // namespace

GitPanel::GitPanel(const QString &projectRoot, QWidget *parent, std::function<bool()> beforeAdd)
    : QWidget(parent),
      projectRoot_(projectRoot),
      history_(new QTreeWidget),
      toolbar_(new QWidget(this)),
      actions_(new GitActions(projectRoot_, toolbar_, std::move(beforeAdd))),
      historyStatus_(new QLabel(tr("Git history")))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(5);
    auto *navigation = new QHBoxLayout(toolbar_);
    navigation->setContentsMargins(0, 0, 0, 0);
    navigation->setSpacing(4);
    navigation->addWidget(actions_);
    layout->addWidget(toolbar_, 0, Qt::AlignRight);
    historyStatus_->setObjectName(QStringLiteral("muted"));
    historyStatus_->setTextFormat(Qt::PlainText);
    layout->addWidget(historyStatus_);

    history_->setObjectName(QStringLiteral("commitHistory"));
    history_->setAccessibleName(tr("Commit history"));
    history_->setRootIsDecorated(false);
    history_->setIndentation(0);
    history_->setUniformRowHeights(true);
    history_->setAlternatingRowColors(true);
    history_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    history_->installEventFilter(this);
    history_->viewport()->installEventFilter(this);
    history_->setItemDelegate(new HistoryDelegate(history_));
    history_->setHeaderLabels({tr("Graph"), tr("Refs"), tr("Commit"), tr("Author"), tr("Date"),
                               tr("Changes"), tr("Files"), tr("Hash")});
    auto *header = history_->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(refsColumn, QHeaderView::ResizeToContents);
    header->setResizeContentsPrecision(-1);
    header->setSectionResizeMode(subjectColumn, QHeaderView::Stretch);
    header->setSectionResizeMode(changesColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(filesColumn, QHeaderView::ResizeToContents);
    history_->headerItem()->setToolTip(changesColumn, tr("Lines added and removed"));
    history_->headerItem()->setTextAlignment(filesColumn, Qt::AlignCenter);
    history_->headerItem()->setToolTip(filesColumn, tr("Number of changed files"));
    history_->setColumnWidth(0, 58);
    history_->setColumnWidth(authorColumn, 120);
    history_->setColumnWidth(dateColumn, 104);
    history_->setColumnWidth(hashColumn, 76);

    layout->addWidget(history_, 1);

    historyTimer_.setInterval(2000);
    connect(&historyTimer_, &QTimer::timeout, this, &GitPanel::checkHistory);
    connect(actions_, &GitActions::repositoryChanged, this, &GitPanel::checkHistory);
    auto *scrollBar = history_->verticalScrollBar();
    connect(scrollBar, &QScrollBar::valueChanged, this, &GitPanel::loadOlderAtBottom);
    connect(scrollBar, &QScrollBar::rangeChanged, this, [this] {
        QTimer::singleShot(0, this, &GitPanel::loadOlderAtBottom);
    });
}

GitPanel::~GitPanel()
{
    history_->removeEventFilter(this);
    history_->viewport()->removeEventFilter(this);
    for (auto *process : findChildren<QProcess *>(Qt::FindDirectChildrenOnly)) {
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(200);
        }
    }
}

void GitPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    checkHistory();
    historyTimer_.start();
}

void GitPanel::hideEvent(QHideEvent *event)
{
    historyTimer_.stop();
    QWidget::hideEvent(event);
}

bool GitPanel::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == history_ || watched == history_->viewport()) && event->type() == QEvent::ContextMenu) {
        const auto *context = static_cast<QContextMenuEvent *>(event);
        if (context->reason() == QContextMenuEvent::Keyboard) {
            const auto index = history_->currentIndex();
            if (index.isValid()) {
                const QRect row = history_->visualRect(index.siblingAtColumn(subjectColumn));
                showCommitMenu(index, history_->viewport()->mapToGlobal(row.center()));
            }
        }
        // Both mouse buttons are handled on release. Supersede the native right-click event.
        return true;
    }
    if (watched == history_->viewport()) {
        if (event->type() == QEvent::MouseButtonPress) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            const auto modifiers = mouse->modifiers();
            pressPosition_ = mouse->position().toPoint();
            pressedCommit_ = history_->indexAt(pressPosition_).siblingAtColumn(0);
            pressedButton_ = modifiers == Qt::NoModifier ? mouse->button() : Qt::NoButton;
            selectingSquashRange_ = pressedCommit_.isValid() && mouse->button() == Qt::LeftButton &&
                modifiers.testFlag(Qt::ShiftModifier) && !(modifiers & (Qt::ControlModifier | Qt::MetaModifier));
        } else if (event->type() == QEvent::MouseMove) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if ((mouse->position().toPoint() - pressPosition_).manhattanLength() >= QApplication::startDragDistance())
                pressedButton_ = Qt::NoButton;
        } else if (event->type() == QEvent::MouseButtonRelease) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton && selectingSquashRange_) {
                selectingSquashRange_ = false;
                QTimer::singleShot(0, this, &GitPanel::squashSelectedCommits);
            } else if ((mouse->button() == Qt::LeftButton || mouse->button() == Qt::RightButton) &&
                       mouse->button() == pressedButton_ && mouse->modifiers() == Qt::NoModifier &&
                       pressedCommit_.isValid() &&
                       history_->indexAt(mouse->position().toPoint()).siblingAtColumn(0) == pressedCommit_ &&
                       (mouse->position().toPoint() - pressPosition_).manhattanLength() < QApplication::startDragDistance()) {
                const auto index = pressedCommit_;
                const auto popup = mouse->globalPosition().toPoint();
                QTimer::singleShot(0, this, [this, index, popup] { showCommitMenu(index, popup); });
            }
            pressedButton_ = Qt::NoButton;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void GitPanel::showCommitMenu(const QModelIndex &index, const QPoint &globalPosition)
{
    const QString hash = index.siblingAtColumn(0).data(Qt::UserRole).toString();
    if (hash.isEmpty())
        return;

    const QString subject = index.siblingAtColumn(subjectColumn).data(Qt::DisplayRole).toString();
    QMenu menu(this);
    auto *cherryPick = menu.addAction(tr("Cherry-pick"));
    auto *checkout = menu.addAction(tr("Checkout"));
    auto *viewChanges = menu.addAction(tr("View Changes"));
    auto *chosen = menu.exec(globalPosition);
    if (chosen == cherryPick)
        actions_->cherryPickCommit(hash);
    else if (chosen == checkout)
        actions_->checkoutCommit(hash);
    else if (chosen == viewChanges)
        emit viewChangesRequested(gitRoot_, hash, subject);
}

void GitPanel::squashSelectedCommits()
{
    if (history_->selectedItems().size() < 2)
        return;

    QVector<GitSquash::Commit> commits;
    for (int index = 0; index < history_->topLevelItemCount(); ++index) {
        const auto *item = history_->topLevelItem(index);
        if (item->isSelected())
            commits.append({item->data(0, Qt::UserRole).toString(), item->text(subjectColumn)});
    }
    GitSquash::request(this, gitRoot_, commits, [this] { checkHistory(); });
}

void GitPanel::updateHistoryStatus(const QString &status)
{
    historyStatus_->setText(status);
    historyStatus_->setVisible(!status.isEmpty());
}

void GitPanel::checkHistory()
{
    if (!isVisible() || historyCommand_)
        return;

    runGit(historyCommand_, projectRoot_,
           {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")}, historyLimit,
           [this](const GitResult &result) {
        if (!result.error.isEmpty() || result.truncated) {
            gitRoot_.clear();
            shallowFile_.clear();
            historyTips_.clear();
            historyCurrent_ = false;
            hasOlder_ = false;
            cancel(statsCommand_);
            history_->clear();
            updateHistoryStatus(tr("Git history is unavailable"));
            historyStatus_->setToolTip(result.error);
            return;
        }

        auto output = result.output;
        if (output.endsWith('\n'))
            output.chop(1);
        const auto root = QString::fromUtf8(output);
        if (root == gitRoot_) {
            readRefs();
            return;
        }

        gitRoot_ = root;
        historyCurrent_ = false;
        hasOlder_ = false;
        runGit(historyCommand_, gitRoot_,
               {QStringLiteral("rev-parse"), QStringLiteral("--path-format=absolute"),
                QStringLiteral("--git-path"), QStringLiteral("shallow")}, historyLimit,
               [this](const GitResult &path) {
            auto bytes = path.output;
            if (bytes.endsWith('\n'))
                bytes.chop(1);
            shallowFile_ = path.error.isEmpty() && !path.truncated ? QString::fromUtf8(bytes) : QString{};
            readRefs();
        });
    });
}

void GitPanel::readRefs()
{
    QStringList arguments{QStringLiteral("for-each-ref"), QStringLiteral("--sort=refname"),
                          QStringLiteral("--format=%(refname)%00%(objectname)%00%(symref)%00%(HEAD)")};
    if (supportsRootRefs_)
        arguments << QStringLiteral("--include-root-refs") << QStringLiteral("HEAD");
    arguments << QStringLiteral("refs");
    runGit(historyCommand_, gitRoot_, arguments, historyLimit, [this](const GitResult &result) {
        if (supportsRootRefs_ && result.error.contains(QStringLiteral("include-root-refs"))) {
            supportsRootRefs_ = false;
            readRefs();
            return;
        }
        if (!result.error.isEmpty() || result.truncated) {
            updateHistoryStatus(tr("Could not check Git history"));
            historyStatus_->setToolTip(result.error);
            return;
        }
        if (supportsRootRefs_) {
            refreshIfChanged(result.output);
            return;
        }
        // The HEAD atom tracks branch switches on older Git. Read the hash for detached HEAD.
        runGit(historyCommand_, gitRoot_,
               {QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("--quiet"),
                QStringLiteral("HEAD")}, historyLimit,
               [this, refs = result.output](const GitResult &head) {
            refreshIfChanged(refs + '\0' + head.output);
        });
    });
}

void GitPanel::refreshIfChanged(QByteArray refs)
{
    QFile shallow(shallowFile_);
    if (!shallowFile_.isEmpty() && shallow.open(QIODevice::ReadOnly))
        refs.append('\0').append(shallow.read(historyLimit));
    if (historyCurrent_ && refs == observedRefs_) {
        loadOlderAtBottom();
        return;
    }

    observedRefs_ = std::move(refs);
    historyCurrent_ = false;
    // Freeze the commit tips so later scroll batches use the same snapshot.
    runGit(historyCommand_, gitRoot_,
           {QStringLiteral("rev-list"), QStringLiteral("--no-walk=unsorted"), QStringLiteral("--all")}, historyLimit,
           [this](const GitResult &result) {
        if (!result.error.isEmpty() || result.truncated) {
            updateHistoryStatus(tr("Could not load Git history"));
            historyStatus_->setToolTip(result.error);
            return;
        }
        historyTips_ = result.output;
        if (historyTips_.isEmpty()) {
            appendHistory({}, pageSize, true, false);
            historyCurrent_ = true;
        } else {
            loadHistory(0, qMax(pageSize, history_->topLevelItemCount()));
        }
    });
}

void GitPanel::loadHistory(int offset, int limit)
{
    if (offset == 0 && history_->topLevelItemCount() == 0)
        updateHistoryStatus(tr("Loading Git history…"));
    historyStatus_->setToolTip({});

    QStringList arguments = {QStringLiteral("log"), QStringLiteral("--stdin"),
                             QStringLiteral("--topo-order"), QStringLiteral("--color=never"),
                             QStringLiteral("--no-show-signature"), QStringLiteral("--no-notes"),
                             QStringLiteral("--decorate=full"),
                             QStringLiteral("--format=%x00%H%x00%h%x00%aN%x00%as%x00%D%x00%s%x00%P%x00"),
                             QStringLiteral("--max-count=%1").arg(limit + 1),
                             QStringLiteral("--skip=%1").arg(offset)};
    runGit(historyCommand_, gitRoot_, arguments, historyLimit,
           [this, offset, limit](const GitResult &result) {
        if (!result.error.isEmpty()) {
            hasOlder_ = false;
            historyCurrent_ = false;
            updateHistoryStatus(offset == 0 ? tr("Could not load Git history")
                                            : tr("Could not load older commits. Retrying automatically."));
            historyStatus_->setToolTip(result.error);
            return;
        }

        appendHistory(result.output, limit, offset == 0, result.truncated);
        historyCurrent_ = true;
    }, historyTips_);
}

void GitPanel::appendHistory(const QByteArray &output, int limit, bool replace, bool truncated)
{
    const auto hashFor = [](const QTreeWidgetItem *item) {
        return item ? item->data(0, Qt::UserRole).toString() : QString{};
    };
    // Scrollbar signals can fire while the tree is being rebuilt.
    hasOlder_ = false;
    QString currentHash;
    QString topHash;
    QSet<QString> selected;
    if (replace) {
        cancel(statsCommand_);
        currentHash = hashFor(history_->currentItem());
        for (const auto *item : history_->selectedItems())
            selected.insert(hashFor(item));
        if (history_->verticalScrollBar()->value() > 0)
            topHash = hashFor(history_->itemAt(0, 0));
        lanes_.clear();
        nextLaneColor_ = 0;
        graphWidth_ = 58;
    }
    const int firstRow = replace ? 0 : history_->topLevelItemCount();
    QTreeWidgetItem *firstCommit = nullptr;
    QTreeWidgetItem *currentCommit = nullptr;
    QTreeWidgetItem *topCommit = nullptr;
    int commits = 0;
    bool hasOlder = false;
    const int previousGraphWidth = graphWidth_;
    {
        QSignalBlocker updating(history_);
        if (replace)
            history_->clear();
        const QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        for (const QByteArray &line : output.split('\n')) {
            if (line.isEmpty())
                continue;
            const QList<QByteArray> fields = line.split('\0');
            if (fields.size() != 9 || !fields[8].isEmpty() || fields[1].isEmpty())
                continue;
            if (commits == limit) {
                hasOlder = true;
                break;
            }

            auto *item = new HistoryItem(history_);
            const int previousLaneCount = int(lanes_.size());
            setGraphRow(*item, fields[1], fields[7]);
            const int columnCount = std::max({int(lanes_.size()), previousLaneCount, item->node.lane + 1});
            graphWidth_ = qMax(graphWidth_, 2 * graphMargin + (columnCount - 1) * laneSpacing);
            item->refs = refLabels(fields[5]);
            const QString hash = QString::fromLatin1(fields[1]);
            const QString subject = QString::fromUtf8(fields[6]);
            item->setData(0, Qt::UserRole, hash);
            item->setText(subjectColumn, subject);
            QStringList refs;
            for (const auto &ref : item->refs)
                refs << ref.name;
            item->setText(refsColumn, refs.join(QStringLiteral(", ")));
            item->setToolTip(refsColumn, refs.join(u'\n'));
            item->setToolTip(subjectColumn, subject);
            item->setText(authorColumn, QString::fromUtf8(fields[3]));
            item->setToolTip(authorColumn, item->text(authorColumn));
            item->setText(dateColumn, QString::fromLatin1(fields[4]));
            item->setText(changesColumn, QStringLiteral("—"));
            item->setText(filesColumn, QStringLiteral("—"));
            item->setTextAlignment(filesColumn, Qt::AlignCenter);
            item->setText(hashColumn, QString::fromLatin1(fields[2]));
            item->setFont(hashColumn, fixedFont);
            item->setToolTip(hashColumn, hash);
            if (!firstCommit)
                firstCommit = item;
            if (replace) {
                item->setSelected(selected.contains(hash));
                if (hash == currentHash)
                    currentCommit = item;
                if (hash == topHash)
                    topCommit = item;
            }
            ++commits;
        }

        const int preferredWidth = qMin(graphWidth_, 206);
        if (firstRow == 0 || preferredWidth > qMin(previousGraphWidth, 206))
            history_->setColumnWidth(0, firstRow == 0 ? preferredWidth : qMax(history_->columnWidth(0), preferredWidth));
        history_->headerItem()->setToolTip(0, graphWidth_ > 206 ? tr("Widen the graph column to show all branches.") : QString{});
        if (replace && firstCommit) {
            if (currentCommit)
                history_->setCurrentItem(currentCommit, 0, QItemSelectionModel::NoUpdate);
            else if (history_->selectedItems().isEmpty())
                history_->setCurrentItem(firstCommit);
            if (topCommit)
                history_->scrollToItem(topCommit, QAbstractItemView::PositionAtTop);
            else
                history_->scrollToTop();
        }
    }

    hasOlder_ = hasOlder && !truncated;
    updateHistoryStatus(truncated ? tr("Git history output was truncated")
                                  : history_->topLevelItemCount() == 0 ? tr("No commits") : QString{});
    loadChangeCounts(firstRow);
}

void GitPanel::loadOlderAtBottom()
{
    if (!hasOlder_ || historyCommand_ || statsCommand_ || !history_->isVisible())
        return;

    const auto *scrollBar = history_->verticalScrollBar();
    if (scrollBar->value() < scrollBar->maximum())
        return;

    const int count = history_->topLevelItemCount();
    if (scrollBar->maximum() == scrollBar->minimum()) {
        const QRect last = history_->visualItemRect(history_->topLevelItem(count - 1));
        if (!last.isValid() || last.bottom() >= history_->viewport()->height())
            return;
    }
    loadHistory(count, pageSize);
}

void GitPanel::loadChangeCounts(int firstRow)
{
    if (firstRow == history_->topLevelItemCount())
        return;

    // Ask Git for totals only, batching the current page in one background command.
    QHash<QByteArray, QTreeWidgetItem *> rows;
    QByteArray commits;
    for (int row = firstRow; row < history_->topLevelItemCount(); ++row) {
        auto *item = history_->topLevelItem(row);
        const auto hash = item->data(0, Qt::UserRole).toString().toLatin1();
        rows.insert(hash, item);
        commits.append(hash).append('\n');
    }
    const QStringList arguments{QStringLiteral("log"), QStringLiteral("--format=%H"),
        QStringLiteral("--no-show-signature"), QStringLiteral("--no-notes"), QStringLiteral("--root"),
        QStringLiteral("--diff-merges=first-parent"), QStringLiteral("--find-renames"),
        QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"), QStringLiteral("--no-relative"),
        QStringLiteral("--color=never"), QStringLiteral("--no-walk=unsorted"), QStringLiteral("--stdin"),
        QStringLiteral("--shortstat"), QStringLiteral("--")};
    runGit(statsCommand_, gitRoot_, arguments, historyLimit, [this, rows](const GitResult &result) {
        QTimer::singleShot(0, this, &GitPanel::loadOlderAtBottom);
        if (!result.error.isEmpty() || result.truncated) {
            for (auto *item : rows) {
                item->setToolTip(changesColumn, tr("Could not load change counts."));
                item->setToolTip(filesColumn, tr("Could not load change counts."));
            }
            return;
        }

        // With LC_ALL=C shortstat omits zero insertions or deletions and has no line for empty commits.
        static const QRegularExpression stat(QStringLiteral(
            R"(^\s*(\d+) files? changed(?:, (\d+) insertions?\(\+\))?(?:, (\d+) deletions?\(-\))?\s*$)"));
        QTreeWidgetItem *current = nullptr;
        QString added;
        QString removed;
        QString files;
        const auto showCounts = [&] {
            if (!current)
                return;
            const QStringList counts{
                QStringLiteral("+") + added, QStringLiteral("−") + removed};
            current->setText(changesColumn, counts.join(QStringLiteral("  ")));
            current->setData(changesColumn, changeCountsRole, counts);
            current->setText(filesColumn, files);
        };
        for (const auto &line : result.output.split('\n')) {
            if (line.isEmpty())
                continue;
            if (auto *item = rows.value(line)) {
                showCounts();
                current = item;
                added = removed = files = QStringLiteral("0");
                continue;
            }
            const auto match = stat.match(QString::fromLatin1(line));
            if (!match.hasMatch()) {
                if (current) {
                    current->setToolTip(changesColumn, tr("Could not read change counts."));
                    current->setToolTip(filesColumn, tr("Could not read change counts."));
                }
                current = nullptr;
                continue;
            }
            files = match.captured(1);
            added = match.hasCaptured(2) ? match.captured(2) : QStringLiteral("0");
            removed = match.hasCaptured(3) ? match.captured(3) : QStringLiteral("0");
        }
        showCounts();
    }, commits);
}

void GitPanel::runGit(QProcess *&active, const QString &directory, const QStringList &arguments,
                      qsizetype maxBytes, ResultHandler handler, const QByteArray &input)
{
    cancel(active);
    auto *process = new GitCommand(this);
    active = process;
    auto **slot = &active;
    process->setWorkingDirectory(directory);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_NO_LAZY_FETCH"), QStringLiteral("1"));
    if (&active == &statsCommand_)
        environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    environment.remove(QStringLiteral("GIT_DIR"));
    environment.remove(QStringLiteral("GIT_WORK_TREE"));
    process->setProcessEnvironment(environment);

    auto readOutput = [process, maxBytes] {
        const auto chunk = process->readAllStandardOutput();
        const bool exceedsLimit = chunk.size() > maxBytes - process->output.size();
        collect(process->output, chunk, maxBytes);
        if (!process->truncated && exceedsLimit) {
            process->truncated = true;
            process->kill();
        }
    };

    connect(process, &QProcess::readyReadStandardOutput, this, readOutput);
    connect(process, &QProcess::readyReadStandardError, this, [process] {
        collect(process->error, process->readAllStandardError(), errorLimit);
    });
    connect(process, &QProcess::finished, this,
            [process, slot, handler, readOutput](int exitCode, QProcess::ExitStatus exitStatus) {
        process->deleteLater();
        if (*slot != process)
            return;
        *slot = nullptr;
        readOutput();
        GitResult result{process->output, {}, process->truncated};
        if (!result.truncated && (exitCode != 0 || exitStatus != QProcess::NormalExit)) {
            result.error = QString::fromUtf8(process->error).trimmed();
            if (result.error.isEmpty())
                result.error = process->errorString();
        }
        handler(result);
    });
    connect(process, &QProcess::errorOccurred, this, [process, slot, handler](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        if (*slot == process) {
            *slot = nullptr;
            handler({{}, process->errorString()});
        }
    });
    QStringList gitArguments{QStringLiteral("--no-pager")};
    gitArguments << arguments;
    if (input.isEmpty()) {
        process->start(QStringLiteral("git"), gitArguments, QIODevice::ReadOnly);
    } else {
        connect(process, &QProcess::started, this, [process, input] {
            process->write(input);
            process->closeWriteChannel();
        });
        process->start(QStringLiteral("git"), gitArguments);
    }
}

void GitPanel::cancel(QProcess *&active)
{
    if (!active)
        return;
    auto *process = active;
    active = nullptr;
    process->kill();
}
