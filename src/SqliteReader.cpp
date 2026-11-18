#include "SqliteReader.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <sqlite3.h>
#include <sys/stat.h>

#include <algorithm>
#include <utility>

namespace {
constexpr qint64 queryTimeoutMs = 1500;
constexpr qsizetype previewCharacters = 4096;
constexpr qsizetype previewBytes = previewCharacters * 4 + 1;
constexpr qsizetype pageBudget = 4 * 1024 * 1024;

class Statement final
{
public:
    ~Statement() { sqlite3_finalize(value); }
    sqlite3_stmt *value = nullptr;
};

QString quoted(const QString &name)
{
    auto result = name;
    result.replace(u'"', QStringLiteral("\"\""));
    return u'"' + result + u'"';
}

QString columnText(sqlite3_stmt *statement, int column)
{
    const auto *text = sqlite3_column_text(statement, column);
    return text ? QString::fromUtf8(reinterpret_cast<const char *>(text), sqlite3_column_bytes(statement, column))
                : QString{};
}

QString databaseError(sqlite3 *database, int code)
{
    if (code == SQLITE_BUSY || code == SQLITE_LOCKED)
        return SqliteReader::tr("The database is busy. Retrying on the next refresh.");
    if (code == SQLITE_INTERRUPT)
        return SqliteReader::tr("The database query was interrupted or took too long.");
    return QString::fromUtf8(sqlite3_errmsg(database));
}

bool prepare(sqlite3 *database, const QString &sql, Statement &statement, QString &error)
{
    const auto bytes = sql.toUtf8();
    const auto code = sqlite3_prepare_v2(database, bytes.constData(), bytes.size(), &statement.value, nullptr);
    if (code == SQLITE_OK)
        return true;
    error = databaseError(database, code);
    return false;
}

bool execute(sqlite3 *database, const char *sql, QString &error)
{
    const auto code = sqlite3_exec(database, sql, nullptr, nullptr, nullptr);
    if (code == SQLITE_OK)
        return true;
    error = databaseError(database, code);
    return false;
}

QString rowOrder(sqlite3 *database, const QString &table, const QStringList &columns, bool view,
                 QString &error)
{
    if (view)
        return {};

    Statement schema;
    if (!prepare(database, QStringLiteral("PRAGMA main.table_xinfo(%1)").arg(quoted(table)), schema, error))
        return {};
    QList<std::pair<int, QString>> keys;
    int code;
    while ((code = sqlite3_step(schema.value)) == SQLITE_ROW) {
        const auto order = sqlite3_column_int(schema.value, 5);
        if (order > 0)
            keys.append({order, columnText(schema.value, 1)});
    }
    if (code != SQLITE_DONE) {
        error = databaseError(database, code);
        return {};
    }
    std::ranges::sort(keys, {}, &std::pair<int, QString>::first);
    QStringList order;
    for (const auto &key : keys)
        order.append(quoted(key.second));
    if (!order.isEmpty())
        return QStringLiteral(" ORDER BY ") + order.join(u',');

    for (const auto alias : {QStringLiteral("rowid"), QStringLiteral("_rowid_"), QStringLiteral("oid")}) {
        if (columns.contains(alias, Qt::CaseInsensitive))
            continue;
        Statement check;
        QString ignored;
        if (prepare(database, QStringLiteral("SELECT %1 FROM main.%2 LIMIT 0").arg(alias, quoted(table)), check, ignored))
            return QStringLiteral(" ORDER BY ") + alias;
    }
    return {};
}

SqliteCell previewCell(sqlite3_stmt *statement, int column, qsizetype &budget)
{
    const auto tagged = columnText(statement, column);
    if (tagged.startsWith(u'n'))
        return {QStringLiteral("NULL"), SqliteCell::Kind::Null};
    if (tagged.startsWith(u'b'))
        return {SqliteReader::tr("BLOB (%1 bytes)").arg(tagged.mid(1)), SqliteCell::Kind::Blob};

    auto text = tagged.mid(1);
    const auto limit = qMin(previewCharacters, qMax<qsizetype>(0, budget / qsizetype(sizeof(QChar))));
    const bool truncated = text.size() > limit;
    if (truncated) {
        auto end = limit;
        if (end > 0 && text.at(end - 1).isHighSurrogate())
            --end;
        text.truncate(end);
        text.append(QChar(0x2026));
    }
    budget -= text.size() * qsizetype(sizeof(QChar));
    return {std::move(text), SqliteCell::Kind::Value, truncated};
}
}

SqliteReader::SqliteReader(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<SqliteSnapshot>();
}

SqliteReader::~SqliteReader()
{
    close();
}

void SqliteReader::close()
{
    if (database_)
        sqlite3_close(database_);
    database_ = nullptr;
    path_.clear();
    dataVersion_ = -1;
    previous_ = {};
}

int SqliteReader::interrupt(void *context)
{
    const auto *reader = static_cast<SqliteReader *>(context);
    return QThread::currentThread()->isInterruptionRequested() ||
           QDateTime::currentMSecsSinceEpoch() >= reader->deadline_;
}

bool SqliteReader::open(const QString &path, QString &error)
{
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    struct stat identity {};
    const auto bytes = QFile::encodeName(canonical);
    if (path.contains(QChar::Null) || canonical.isEmpty() || !info.isFile() ||
        ::stat(bytes.constData(), &identity) != 0) {
        close();
        error = tr("The database file is missing or unavailable.");
        return false;
    }
    if (database_ && path_ == canonical && device_ == quint64(identity.st_dev) && inode_ == quint64(identity.st_ino))
        return true;

    close();
    const auto code = sqlite3_open_v2(bytes.constData(), &database_, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    if (code != SQLITE_OK) {
        error = database_ ? databaseError(database_, code) : tr("Could not open the SQLite database.");
        close();
        return false;
    }
    path_ = canonical;
    device_ = quint64(identity.st_dev);
    inode_ = quint64(identity.st_ino);
    sqlite3_busy_timeout(database_, 150);
    sqlite3_progress_handler(database_, 1000, &SqliteReader::interrupt, this);
    execute(database_, "PRAGMA cache_size=-1024", error);
    return error.isEmpty();
}

void SqliteReader::request(const QString &path, const QString &table, qint64 offset,
                           quint64 generation, bool force)
{
    deadline_ = QDateTime::currentMSecsSinceEpoch() + queryTimeoutMs;
    QString error;
    if (!open(path, error)) {
        emit failed(generation, error);
        return;
    }
    Statement version;
    if (!prepare(database_, QStringLiteral("PRAGMA data_version"), version, error)) {
        dataVersion_ = -1;
        emit failed(generation, error);
        return;
    }
    const auto code = sqlite3_step(version.value);
    if (code != SQLITE_ROW) {
        dataVersion_ = -1;
        emit failed(generation, databaseError(database_, code));
        return;
    }
    const auto currentVersion = sqlite3_column_int64(version.value, 0);
    // Finalize the pragma before a transaction so it cannot retain a read lock between polls.
    sqlite3_finalize(std::exchange(version.value, nullptr));
    const auto requestedOffset = qMax<qint64>(0, offset);
    if (!force && currentVersion == dataVersion_ && table == previous_.table && requestedOffset == previous_.offset) {
        auto result = previous_;
        result.generation = generation;
        result.changed = false;
        emit ready(result);
        return;
    }

    SqliteSnapshot snapshot;
    snapshot.path = path_;
    snapshot.table = table;
    snapshot.offset = requestedOffset;
    snapshot.generation = generation;
    if (!execute(database_, "BEGIN", error)) {
        dataVersion_ = -1;
        emit failed(generation, error);
        return;
    }
    const bool loaded = readSnapshot(snapshot, error);
    QString rollbackError;
    execute(database_, "ROLLBACK", rollbackError);
    if (!loaded || !rollbackError.isEmpty()) {
        dataVersion_ = -1;
        if (!rollbackError.isEmpty())
            close();
        if (!loaded && rollbackError.isEmpty() && !snapshot.tables.isEmpty()) {
            snapshot.rows.clear();
            snapshot.hasMore = false;
            snapshot.error = error;
            emit ready(snapshot);
        } else {
            emit failed(generation, !error.isEmpty() ? error : rollbackError);
        }
        return;
    }
    previous_ = snapshot;
    dataVersion_ = currentVersion;
    emit ready(snapshot);
}

bool SqliteReader::readSnapshot(SqliteSnapshot &snapshot, QString &error)
{
    Statement tables;
    if (!prepare(database_, QStringLiteral("SELECT name,type FROM main.sqlite_schema "
                                         "WHERE type IN ('table','view') AND substr(name,1,7) <> 'sqlite_' "
                                         "ORDER BY name COLLATE NOCASE"), tables, error))
        return false;
    int code;
    while ((code = sqlite3_step(tables.value)) == SQLITE_ROW)
        snapshot.tables.append({columnText(tables.value, 0), columnText(tables.value, 1) == QStringLiteral("view")});
    if (code != SQLITE_DONE) {
        error = databaseError(database_, code);
        return false;
    }
    auto table = std::ranges::find(snapshot.tables, snapshot.table, &SqliteTable::name);
    if (table == snapshot.tables.end()) {
        if (snapshot.tables.isEmpty()) {
            snapshot.table.clear();
            snapshot.offset = 0;
            return true;
        }
        table = snapshot.tables.begin();
        snapshot.table = table->name;
        snapshot.offset = 0;
    }

    Statement columns;
    const auto from = QStringLiteral(" FROM main.") + quoted(snapshot.table);
    if (!prepare(database_, QStringLiteral("SELECT *") + from + QStringLiteral(" LIMIT 0"), columns, error))
        return false;
    QStringList expressions;
    const auto count = sqlite3_column_count(columns.value);
    for (int column = 0; column < count; ++column) {
        const auto name = QString::fromUtf8(sqlite3_column_name(columns.value, column));
        snapshot.columns.append(name);
        const auto field = quoted(name);
        // Tag each value while limiting text and fetching only the size of a BLOB.
        expressions.append(QStringLiteral("CASE typeof(%1) WHEN 'null' THEN 'n' "
                                          "WHEN 'blob' THEN 'b'||length(%1) "
                                          "WHEN 'text' THEN 't'||substr(CAST(%1 AS BLOB),1,%2) "
                                          "ELSE 'v'||CAST(%1 AS TEXT) END").arg(field).arg(previewBytes));
    }
    const auto order = rowOrder(database_, snapshot.table, snapshot.columns, table->view, error);
    if (!error.isEmpty())
        return false;
    Statement rows;
    if (!prepare(database_, QStringLiteral("SELECT ") + expressions.join(u',') + from + order +
                  QStringLiteral(" LIMIT ? OFFSET ?"), rows, error))
        return false;
    sqlite3_bind_int(rows.value, 1, SqliteSnapshot::pageSize + 1);
    sqlite3_bind_int64(rows.value, 2, snapshot.offset);
    qsizetype budget = pageBudget;
    while ((code = sqlite3_step(rows.value)) == SQLITE_ROW) {
        const auto rowCost = count * qsizetype(sizeof(SqliteCell));
        if (snapshot.rows.size() == SqliteSnapshot::pageSize || (!snapshot.rows.isEmpty() && budget <= rowCost)) {
            snapshot.hasMore = true;
            break;
        }
        budget -= rowCost;
        QList<SqliteCell> cells;
        cells.reserve(count);
        for (int column = 0; column < count; ++column)
            cells.append(previewCell(rows.value, column, budget));
        snapshot.rows.append(std::move(cells));
    }
    if (code != SQLITE_ROW && code != SQLITE_DONE) {
        error = databaseError(database_, code);
        return false;
    }
    return true;
}
