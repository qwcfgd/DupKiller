#pragma once
#include "executor.h"
#include "filetreemodel.h"
#include <QFutureWatcher>

namespace dup {
class ViewModel final : public QObject {
    Q_OBJECT
public:
    explicit ViewModel(QObject *parent = nullptr);
    ~ViewModel() override;
    FileTreeModel *model() { return &tree; }
    bool busy() const { return working; }
    bool canExecute() const { return !working && !stale && tree.operationCount() > 0; }
    void scan(ScanOptions options);
    void execute(bool permanent);
    void recover(const QString &journal);
    void cancel();
    void invalidate();
    QString logsDirectory() const;
signals:
    void busyChanged(bool busy);
    void summaryChanged(const QString &summary);
    void progressChanged(const QString &phase, quint64 completed, quint64 total);
    void message(const QString &text);
    void scanReady();
private:
    FileTreeModel tree;
    QFutureWatcher<ScanResult> scanWatcher;
    QFutureWatcher<ExecutionResult> operationWatcher;
    Cancel cancellation;
    bool working = false, stale = true;
    void setBusy(bool busy);
    void summarize();
    Progress progressCallback();
};
}
