#pragma once

#include <QJsonObject>
#include <QMainWindow>
#include <QStringList>
#include <QVector>

class QLabel;
class QCloseEvent;
class QStackedWidget;
class QTabBar;
class QToolButton;
class ProjectView;

class Workspace final : public QMainWindow
{
public:
    Workspace();
    static QString stateDirectory();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    enum Group { Cpp, Python, Web, GroupCount };

    struct Project {
        QString root;
        Group group = Cpp;
        QJsonObject state;
        ProjectView *view = nullptr;
    };

    void openProjectDialog(Group group);
    void openSettings();
    void selectGroup(Group group);
    void rebuildTabs();
    void activateTab(int tab);
    void moveProjectTab(int from, int to);
    void removeProject(int tab);
    void readSession();
    bool saveSession();
    void updateSessionStatus(bool saved);

    QVector<Project> projects_;
    QJsonObject closedRunConfigurations_;
    QString lastProject_[GroupCount];
    Group group_ = Cpp;
    QToolButton *groups_[GroupCount]{};
    QLabel *counts_[GroupCount]{};
    QTabBar *tabs_ = nullptr;
    QStackedWidget *configurationStack_ = nullptr;
    QStackedWidget *stack_ = nullptr;
    QWidget *empty_ = nullptr;
    QLabel *emptyTitle_ = nullptr;
    QLabel *activePath_ = nullptr;
    QLabel *sessionStatus_ = nullptr;
};
