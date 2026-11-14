#include "GitFileList.h"

#include <QEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

namespace {
constexpr int statusColorRole = Qt::UserRole + 42;

enum class StatusColor { Added, Removed, Other };

QColor statusColor(StatusColor status, const QColor &background)
{
    const bool dark = background.lightness() < 128;
    switch (status) {
    case StatusColor::Added: return QColor(dark ? "#8de5a6" : "#116425");
    case StatusColor::Removed: return QColor(dark ? "#ff9c9c" : "#a42222");
    case StatusColor::Other: return QColor(dark ? "#9fc7ff" : "#174f96");
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

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (!option.state.testFlag(QStyle::State_Selected) || !option.widget) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem row(option);
        initStyleOption(&row, index);
        const auto *style = row.widget->style();
        const auto rect = style->subElementRect(QStyle::SE_ItemViewItemText, &row, row.widget);
        const auto content = rect.adjusted(3, 0, -3, 0);
        const auto text = row.fontMetrics.elidedText(row.text, row.textElideMode, qMax(0, content.width()));
        row.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem, &row, painter, row.widget);

        painter->save();
        painter->setClipRect(rect, Qt::IntersectClip);
        painter->setFont(row.font);
        const auto status = index.data(statusColorRole);
        const auto background = row.palette.color(QPalette::Highlight);
        painter->setPen(status.isValid() ? statusColor(static_cast<StatusColor>(status.toInt()), background)
                                        : row.palette.color(QPalette::HighlightedText));
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

    const auto color = added ? StatusColor::Added : removed ? StatusColor::Removed : StatusColor::Other;
    item.setData(0, statusColorRole, static_cast<int>(color));
    item.setForeground(0, statusColor(color, tree.palette().color(QPalette::Base)));
    item.setData(0, Qt::AccessibleDescriptionRole, description);
    item.setToolTip(0, item.text(0) + QLatin1Char('\n') + description);
}
