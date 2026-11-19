#pragma once

#include <QJsonObject>
#include <QList>
#include <QThread>
#include <QTimer>
#include <QWidget>

class DatabaseTableModel;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTableView;
class QTreeWidget;
class SqliteReader;
struct SqliteSnapshot;

class DatabasePanel final : public QWidget
{
    Q_OBJECT

public:
    explicit DatabasePanel(const QString &initialDirectory, QWidget *parent = nullptr);
    ~DatabasePanel() override;

    void openDatabase(const QString &path, const QString &name = {});
    void restoreState(const QJsonObject &state);
    QJsonObject saveState() const;

signals:
    void stateChanged();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct Connection {
        QString name;
        QString path;
        QString table;
    };

    QString connectionPath(const QString &path) const;
    void activateConnection(int index);
    void rememberCurrentTable();
    void updateConnections();
    void refresh(bool force = false);
    void startReader();
    void showSnapshot(const SqliteSnapshot &snapshot);
    void finishRequest();
    void updatePath();
    void copyCells();
    void changePage(qint64 offset);

    const QString initialDirectory_;
    QString path_;
    QString table_;
    QList<Connection> connections_;
    qint64 offset_ = 0;
    quint64 generation_ = 0;
    bool busy_ = false;
    bool pendingRefresh_ = false;
    bool pendingForce_ = false;
    bool hasMore_ = false;
    qint64 nextOffset_ = 0;
    QList<qint64> previousOffsets_;
    QThread workerThread_;
    SqliteReader *reader_;
    QTimer timer_;
    QCheckBox *autoRefresh_;
    QComboBox *connectionPicker_;
    QLabel *pathLabel_;
    QLabel *status_;
    QTreeWidget *tables_;
    QTableView *rows_;
    DatabaseTableModel *model_;
    QPushButton *previous_;
    QPushButton *next_;
    QLabel *range_;
};
