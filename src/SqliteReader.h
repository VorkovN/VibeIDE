#pragma once

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QStringList>

struct sqlite3;

struct SqliteTable {
    QString name;
    bool view = false;
};

struct SqliteCell {
    enum class Kind { Value, Null, Blob };
    QString text;
    Kind kind = Kind::Value;
    bool truncated = false;
};

struct SqliteSnapshot {
    static constexpr int pageSize = 200;
    QString path;
    QList<SqliteTable> tables;
    QString table;
    QStringList columns;
    QList<QList<SqliteCell>> rows;
    QString error;
    qint64 offset = 0;
    bool hasMore = false;
    quint64 generation = 0;
    bool changed = true;
};

Q_DECLARE_METATYPE(SqliteSnapshot)

class SqliteReader final : public QObject
{
    Q_OBJECT

public:
    explicit SqliteReader(QObject *parent = nullptr);
    ~SqliteReader() override;

    // Invoke on the reader's worker thread. A request never changes the database.
    void request(const QString &path, const QString &table, qint64 offset,
                 quint64 generation, bool force);

signals:
    void ready(const SqliteSnapshot &snapshot);
    void failed(quint64 generation, const QString &error);

private:
    void close();
    bool open(const QString &path, QString &error);
    bool readSnapshot(SqliteSnapshot &snapshot, QString &error);
    static int interrupt(void *context);

    sqlite3 *database_ = nullptr;
    QString path_;
    quint64 device_ = 0;
    quint64 inode_ = 0;
    qint64 deadline_ = 0;
    qint64 dataVersion_ = -1;
    SqliteSnapshot previous_;
};
