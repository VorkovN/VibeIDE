#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

#include <sys/types.h>

class QSocketNotifier;

class Pty final : public QObject
{
    Q_OBJECT

public:
    explicit Pty(QObject *parent = nullptr);
    ~Pty() override;

    bool start(const QString &directory, int rows, int cols);
    void write(const QByteArray &data);
    void resize(int rows, int cols);
    QString workingDirectory() const;
    QString errorString() const;
    bool isRunning() const;

signals:
    void dataReceived(const QByteArray &data);
    void finished();

private:
    void readAvailable();
    void flushWrites();
    void checkChild();
    void closeMaster();

    QTimer childTimer_;
    QSocketNotifier *readNotifier_ = nullptr;
    QSocketNotifier *writeNotifier_ = nullptr;
    int master_ = -1;
    pid_t child_ = -1;
    QByteArray pendingWrites_;
    mutable QString directory_;
    QString error_;
};
