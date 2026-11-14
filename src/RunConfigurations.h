#pragma once

#include <QJsonObject>
#include <QList>
#include <QProcessEnvironment>
#include <QSet>
#include <QStringList>
#include <QWidget>

class QMenu;
class QPushButton;
class QToolButton;

class RunConfigurations final : public QWidget
{
    Q_OBJECT

public:
    RunConfigurations(const QString &projectRoot, bool isWeb, QWidget *parent = nullptr);

    QJsonObject saveState() const;
    void restoreState(const QJsonObject &state);
    void setRunning(const QString &configurationId, bool running);

signals:
    void stateChanged();
    void runRequested(const QString &program, const QStringList &arguments,
                      const QString &workingDirectory, const QString &title,
                      const QProcessEnvironment &environment, const QString &configurationId);
    void stopRequested(const QString &configurationId);

private:
    enum class Kind { PythonScript = 3, PythonModule = 4, ShellScript = 5 };

    struct Configuration {
        QString id;
        QString name;
        Kind kind = Kind::PythonScript;
        QString program;
        QString entry;
        QString workingDirectory;
        QString arguments;
        bool useUv = false;
        QStringList environment;
    };

    const Configuration *selectedConfiguration() const;
    static QString kindName(Kind kind);
    QString projectPath(const QString &path) const;
    QString commandPath(const QString &program) const;
    void refreshButtons();
    void rebuildMenu();
    void selectConfiguration(const QString &id);
    void editConfiguration(bool create);
    void removeConfiguration();
    void runOrStopSelected();

    QString projectRoot_;
    bool isWeb_;
    QList<Configuration> configurations_;
    QStringList recent_;
    QSet<QString> runningIds_;
    QString selectedId_;
    QMenu *menu_;
    QToolButton *selector_;
    QPushButton *runButton_;
};
