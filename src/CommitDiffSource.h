#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QStringList>

#include <functional>

class QProcess;

class CommitDiffSource final : public QObject
{
    Q_OBJECT

public:
    struct File {
        QByteArray oldPath;
        QByteArray newPath;
        QByteArray oldObject;
        QByteArray newObject;
        QByteArray oldMode;
        QByteArray newMode;
        QByteArray status;
        QByteArray workingPath;
    };

    // Starts use Git's one-based line numbers. An empty range points at the
    // preceding line (zero for an empty range at the beginning).
    struct Hunk {
        int oldStart;
        int oldCount;
        int newStart;
        int newCount;
    };

    struct Preview {
        QString before;
        QString after;
        QList<Hunk> hunks;
        QString unavailableReason;
    };

    explicit CommitDiffSource(const QString &repository, QObject *parent = nullptr);
    ~CommitDiffSource() override;

    void loadFiles(const QString &fullHash);
    void loadPreview(const File &file);
    void loadWorkingFiles();
    void loadWorkingPreview(const File &file);

signals:
    void filesReady(const QList<File> &files);
    void previewReady(const Preview &preview);
    void failed(const QString &error);

private:
    struct Result {
        QByteArray data;
        QString error;
        bool oversized = false;
    };

    using Completion = std::function<void(Result)>;

    void loadAfter(const File &file, Preview preview);
    void loadBlob(const QByteArray &object, std::function<void(QString)> completion);
    void loadHunks(const File &file, Preview preview);
    void loadWorkingBefore(const File &file, Preview preview, bool afterExists);
    void compareWorking(Preview preview, bool beforeExists, bool afterExists);
    void finishComparison(Preview preview, Result result);
    bool decodeBlob(const QByteArray &data, QString &text);
    void unavailable(const QString &reason);
    void run(QStringList arguments, qsizetype limit, Completion completion,
             const QString &directory = {}, bool acceptsExitOne = false);
    void cancel();

    QString repository_;
    QString workingRoot_;
    QProcess *process_ = nullptr;
};
