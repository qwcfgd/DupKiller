#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QStringList>
#include <QMap>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

namespace dup {
enum class Mode { Single, Dual };
enum class Backend { Auto, Native };
enum class KeepRule { Shallowest, Oldest, Newest };
struct FileRecord {
    QString path, root, name, side;
    quint64 size = 0, fileId = 0, volumeId = 0, writeTicks = 0;
    quint32 attributes = 0;
    int depth = 0;
    QDateTime modified;
    QByteArray sha256;
    QMap<QString, QByteArray> streams; // Full SHA-256 for each named NTFS stream.
};
struct DuplicateGroup { QVector<FileRecord> files; int keeper = -1; QVector<bool> replace; };
struct ScanOptions { Mode mode = Mode::Single; Backend backend = Backend::Auto; QString rootA, rootB; };
struct ScanResult {
    ScanOptions options;
    QVector<DuplicateGroup> groups;
    QStringList messages;
    quint64 enumerated = 0, hashed = 0, skipped = 0;
    bool cancelled = false;
    QString error;
};
struct Operation { FileRecord source, target; };
struct ExecutionResult { int done = 0, failed = 0; bool cancelled = false; QString logPath; QStringList messages; };
using Cancel = std::shared_ptr<std::atomic_bool>;
using Progress = std::function<void(const QString &, quint64, quint64)>;
QString cleanPath(const QString &path);
bool samePath(const QString &a, const QString &b);
bool withinRoot(const QString &path, const QString &root);
QString validateRoots(ScanOptions &options);
int pickKeeper(const DuplicateGroup &group, Mode mode, KeepRule rule);
void setKeeper(DuplicateGroup &group, Mode mode, int index);
QString readableSize(quint64 bytes);
}
