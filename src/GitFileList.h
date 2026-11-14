#pragma once

#include <QByteArray>
#include <QString>

class QTreeWidget;
class QTreeWidgetItem;

namespace GitFileList {
void useStatusColors(QTreeWidget &tree);

// Both Git porcelain XY and raw diff statuses (such as R100) are accepted.
void setStatus(QTreeWidgetItem &item, const QTreeWidget &tree, const QByteArray &status);
}
