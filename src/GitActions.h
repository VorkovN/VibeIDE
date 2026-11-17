#pragma once

#include <QByteArray>
#include <QStringList>
#include <QWidget>

#include <functional>

class QProcess;
class QPushButton;

class GitActions final : public QWidget
{
    Q_OBJECT

public:
    explicit GitActions(const QString &projectRoot, QWidget *parent = nullptr,
                        std::function<bool()> beforeAdd = {});
    ~GitActions() override;

    void checkoutCommit(const QString &hash);
    void cherryPickCommit(const QString &hash);

signals:
    void repositoryChanged();

private:
    struct Result {
        QByteArray output;
        QString error;
    };

    using Completion = std::function<void(Result)>;

    void fetch();
    void add();
    void commit();
    void run(QStringList arguments, Completion completion, QByteArray input = {},
             bool structuredOutput = false);
    void setBusy(bool busy);
    void showError(const QString &action, const QString &error);
    void completeAction(const QString &action, QPushButton *button, Result result);

    const QString projectRoot_;
    const std::function<bool()> beforeAdd_;
    QProcess *process_ = nullptr;
    QPushButton *fetch_;
    QPushButton *add_;
    QPushButton *commit_;
};
