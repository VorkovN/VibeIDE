#pragma once

#include <QByteArray>
#include <QPersistentModelIndex>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <functional>

class GitActions;
class QLabel;
class QProcess;
class QShowEvent;
class QTreeWidget;

class GitPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit GitPanel(const QString &projectRoot, QWidget *parent = nullptr);
    ~GitPanel() override;
    QWidget *toolbar() const { return toolbar_; }

signals:
    void viewChangesRequested(const QString &repository, const QString &hash, const QString &subject);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct GitResult {
        QByteArray output;
        QString error;
        bool truncated = false;
    };

    using ResultHandler = std::function<void(const GitResult &)>;

    struct GraphLane {
        QByteArray commit;
        int color = 0;
    };

    struct HistoryItem;
    class HistoryDelegate;

    void checkHistory();
    void readRefs();
    void refreshIfChanged(QByteArray refs);
    void loadHistory(int offset, int limit);
    void appendHistory(const QByteArray &output, int limit, bool replace, bool truncated);
    void loadOlderAtBottom();
    void loadChangeCounts(int firstRow);
    void setGraphRow(HistoryItem &item, const QByteArray &hash, const QByteArray &parentHashes);
    int laneFor(const QByteArray &commit) const;
    int openLane(int from = 0);
    void updateHistoryStatus(const QString &status);
    void showCommitMenu(const QModelIndex &index, const QPoint &globalPosition);
    void squashSelectedCommits();
    void runGit(QProcess *&active, const QString &directory, const QStringList &arguments,
                qsizetype maxBytes, ResultHandler handler, const QByteArray &input = {});
    static void cancel(QProcess *&active);

    const QString projectRoot_;
    QString gitRoot_;
    QString shallowFile_;
    QByteArray observedRefs_;
    QByteArray historyTips_;
    QTimer historyTimer_;
    QList<GraphLane> lanes_;
    int nextLaneColor_ = 0;
    int graphWidth_ = 58;
    bool hasOlder_ = false;
    bool historyCurrent_ = false;
    bool supportsRootRefs_ = true;
    bool selectingSquashRange_ = false;
    QPersistentModelIndex pressedCommit_;
    QPoint pressPosition_;
    Qt::MouseButton pressedButton_ = Qt::NoButton;
    QTreeWidget *history_;
    QWidget *toolbar_;
    GitActions *actions_;
    QLabel *historyStatus_;
    QProcess *historyCommand_ = nullptr;
    QProcess *statsCommand_ = nullptr;
};
