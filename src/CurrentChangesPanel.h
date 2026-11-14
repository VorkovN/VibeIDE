#pragma once

#include <QProcess>
#include <QTimer>
#include <QWidget>

class QLabel;
class QTreeWidget;

class CurrentChangesPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit CurrentChangesPanel(const QString &root, QWidget *parent = nullptr);
    ~CurrentChangesPanel() override;

signals:
    void viewChangesRequested(const QByteArray &path);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void refresh();
    void collectOutput();
    void showChanges();
    void showError(const QString &message);

    QProcess git_;
    QTimer timer_;
    QTreeWidget *files_;
    QLabel *status_;
    QByteArray output_;
    QByteArray errors_;
    QByteArray displayed_;
    bool loaded_ = false;
    bool oversized_ = false;
};
