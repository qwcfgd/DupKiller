#include "viewmodel.h"
#include "scanner.h"
#include <QDir>
#include <QPointer>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>

namespace dup {
ViewModel::ViewModel(QObject *parent) : QObject(parent), tree(this), scanWatcher(this), operationWatcher(this) {
    connect(&tree, &FileTreeModel::planChanged, this, &ViewModel::summarize);
    connect(&scanWatcher, &QFutureWatcher<ScanResult>::finished, this, [this] {
        const auto result = scanWatcher.result();
        tree.setResult(result); stale = result.cancelled || !result.error.isEmpty();
        for (const auto &line : result.messages) emit message(line);
        if (!result.error.isEmpty()) emit message(result.error);
        if (result.cancelled) emit message(QStringLiteral("扫描已取消；未修改任何文件。"));
        setBusy(false); summarize(); emit scanReady();
    });
    connect(&operationWatcher, &QFutureWatcher<ExecutionResult>::finished, this, [this] {
        const auto result = operationWatcher.result();
        for (const auto &line : result.messages) emit message(line);
        emit message(QStringLiteral("完成 %1 项，跳过/失败 %2 项%3。日志：%4").arg(result.done).arg(result.failed)
                     .arg(result.cancelled ? QStringLiteral("，已取消后续操作") : QString()).arg(result.logPath));
        stale = true; setBusy(false); summarize();
    });
}
ViewModel::~ViewModel() { cancel(); scanWatcher.waitForFinished(); operationWatcher.waitForFinished(); }
void ViewModel::setBusy(bool b) { working = b; emit busyChanged(b); }
void ViewModel::cancel() { if (cancellation) cancellation->store(true); }
void ViewModel::invalidate() {
    if (!working && !stale && !tree.result().groups.isEmpty()) {
        stale = true; emit message(QStringLiteral("目录/模式/扫描方式已改变，请重新扫描后执行。")); summarize();
    }
}
QString ViewModel::logsDirectory() const { return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs"; }
Progress ViewModel::progressCallback() {
    const QPointer<ViewModel> self(this);
    return [self](const QString &phase, quint64 completed, quint64 total) {
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, phase, completed, total] { if (self) emit self->progressChanged(phase, completed, total); }, Qt::QueuedConnection);
    };
}
void ViewModel::scan(ScanOptions options) {
    if (working) return;
    cancellation = std::make_shared<std::atomic_bool>(false); stale = true; tree.clear(); setBusy(true);
    emit message(QStringLiteral("开始扫描；名称、大小、SHA-256 必须同时相同。"));
    const auto token = cancellation; const auto progress = progressCallback();
    scanWatcher.setFuture(QtConcurrent::run([options, token, progress] { return Scanner::scan(options, token, progress); }));
}
void ViewModel::execute(bool permanent) {
    if (!canExecute()) return;
    const auto plan = tree.plan(); const auto options = tree.result().options; const auto logs = logsDirectory();
    cancellation = std::make_shared<std::atomic_bool>(false); setBusy(true);
    const auto token = cancellation; const auto progress = progressCallback();
    operationWatcher.setFuture(QtConcurrent::run([plan, options, permanent, logs, token, progress] { return Executor::execute(plan, options, permanent, logs, token, progress); }));
}
void ViewModel::recover(const QString &journal) {
    if (working) return;
    cancellation = std::make_shared<std::atomic_bool>(false); setBusy(true);
    const auto token = cancellation; const auto progress = progressCallback();
    operationWatcher.setFuture(QtConcurrent::run([journal, token, progress] { return Executor::recover(journal, token, progress); }));
}
void ViewModel::summarize() {
    const auto &r = tree.result();
    emit summaryChanged(QStringLiteral("%1 个重复组  ·  %2 份待替换  ·  副本大小 %3  ·  枚举 %4 / 哈希 %5 / 跳过 %6%7")
        .arg(r.groups.size()).arg(tree.plan().size()).arg(readableSize(tree.saving())).arg(r.enumerated).arg(r.hashed).arg(r.skipped)
        .arg(stale && !r.groups.isEmpty() ? QStringLiteral("  ·  请重新扫描") : QString()));
}
}
