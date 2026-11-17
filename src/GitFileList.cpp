#include "GitFileList.h"

#include <QEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

namespace {
constexpr int statusColorRole = Qt::UserRole + 42;

enum class StatusColor { Added, Removed, Conflict, Other };

QColor statusColor(StatusColor status, const QColor &background)
{
    const bool dark = background.lightness() < 128;
    switch (status) {
    case StatusColor::Added: return QColor(dark ? "#86e3ad" : "#167342");
    case StatusColor::Removed: return QColor(dark ? "#ffa2ae" : "#a53549");
    case StatusColor::Conflict: return QColor(dark ? "#e8bd80" : "#82571e");
    case StatusColor::Other: return QColor(dark ? "#a7c7f7" : "#315d9a");
    }
    return {};
}

class StatusDelegate final : public QStyledItemDelegate
{
public:
    explicit StatusDelegate(QTreeWidget &tree) : QStyledItemDelegate(&tree), tree_(tree)
    {
        tree_.installEventFilter(this);
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        if (index.data(statusColorRole).isValid()) {
            size.setWidth(size.width() + 11);
            size.setHeight(qMax(27, size.height()));
        }
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const auto status = index.data(statusColorRole);
        if (!status.isValid() || !option.widget) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem row(option);
        initStyleOption(&row, index);
        const auto *style = row.widget->style();
        const auto rect = style->subElementRect(QStyle::SE_ItemViewItemText, &row, row.widget);
        const auto content = rect.adjusted(15, 0, -6, 0);
        const auto text = row.fontMetrics.elidedText(row.text, row.textElideMode, qMax(0, content.width()));
        row.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem, &row, painter, row.widget);

        painter->save();
        painter->setClipRect(rect, Qt::IntersectClip);
        painter->setFont(row.font);
        const bool selected = row.state.testFlag(QStyle::State_Selected);
        const auto background = row.palette.color(selected ? QPalette::Highlight : QPalette::Base);
        const auto color = statusColor(static_cast<StatusColor>(status.toInt()), background);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawRoundedRect(QRectF(rect.left() + 5, QRectF(rect).center().y() - 5, 3, 10), 1.5, 1.5);
        painter->setPen(color);
        painter->drawText(content, row.displayAlignment, text);
        painter->restore();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == &tree_ && event->type() == QEvent::PaletteChange) {
            const auto background = tree_.palette().color(QPalette::Base);
            for (QTreeWidgetItemIterator item(&tree_); *item; ++item) {
                auto *row = *item;
                const auto status = row->data(0, statusColorRole);
                if (status.isValid())
                    row->setForeground(0, statusColor(static_cast<StatusColor>(status.toInt()), background));
            }
        }
        return QStyledItemDelegate::eventFilter(watched, event);
    }

private:
    QTreeWidget &tree_;
};
}

void GitFileList::useStatusColors(QTreeWidget &tree)
{
    tree.setItemDelegate(new StatusDelegate(tree));
}

void GitFileList::setStatus(QTreeWidgetItem &item, const QTreeWidget &tree, const QByteArray &status)
{
    const bool conflict = status.contains('U') || status == "AA" || status == "DD";
    const bool removed = !conflict && (status.startsWith('D') || status.endsWith('D'));
    const bool added = !conflict && !removed &&
        (status.contains('A') || status.contains('C') || status == "??");

    QString description;
    if (conflict)
        description = QObject::tr("Conflicted");
    else if (removed)
        description = QObject::tr("Deleted");
    else if (status == "??")
        description = QObject::tr("Untracked");
    else if (status.contains('C'))
        description = QObject::tr("Copied");
    else if (added)
        description = QObject::tr("Added");
    else if (status.contains('R'))
        description = QObject::tr("Renamed");
    else
        description = QObject::tr("Modified");

    const auto color = conflict ? StatusColor::Conflict
        : added ? StatusColor::Added : removed ? StatusColor::Removed : StatusColor::Other;
    item.setData(0, statusColorRole, static_cast<int>(color));
    item.setForeground(0, statusColor(color, tree.palette().color(QPalette::Base)));
    item.setData(0, Qt::AccessibleDescriptionRole, description);
    item.setToolTip(0, item.text(0) + QLatin1Char('\n') + description);
}
