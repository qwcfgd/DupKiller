#pragma once
#include "domain.h"
#include <QSet>
#include <windows.h>
#include <vector>

namespace dup::win {
class Handle {
public:
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { reset(); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&h) noexcept : value(h.value) { h.value = INVALID_HANDLE_VALUE; }
    Handle &operator=(Handle &&h) noexcept { if (this != &h) { reset(); value = h.value; h.value = INVALID_HANDLE_VALUE; } return *this; }
    bool valid() const { return value != INVALID_HANDLE_VALUE && value != nullptr; }
    void reset() { if (valid()) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
};
QString errorText(DWORD code = GetLastError());
std::wstring nativePath(const QString &path);
bool safeDirectory(const QString &path, QString &error);
bool lockParents(const QString &file, std::vector<Handle> &locks, QString &error);
bool inspect(const QString &path, const QString &root, const QString &side, FileRecord &file, QString &error);
bool metadata(HANDLE handle, FileRecord &file, QString &error, bool allowSystem = false);
bool unchanged(const FileRecord &a, const FileRecord &b);
bool hasExtraStreams(const QString &path);
bool hashStreams(const QString &path, QMap<QString, QByteArray> &hashes, const Cancel &cancel, QString &error,
                 std::vector<Handle> *guards = nullptr);
bool copyStreams(const QString &source, const QString &destination, const QMap<QString, QByteArray> &expected,
                 const Cancel &cancel, QString &error);
using ReadProgress = std::function<void(quint64)>;
bool hashHandle(HANDLE h, QByteArray &hash, const Cancel &cancel, QString &error, const ReadProgress &readProgress = {});
bool hashFile(FileRecord &file, const Cancel &cancel, QString &error, const ReadProgress &readProgress = {});
bool sampleFile(const FileRecord &file, QByteArray &sample, const Cancel &cancel, QString &error);
int recommendedHashWorkers(const QString &root);
bool renameHandle(HANDLE h, const QString &destination, QString &error);
bool createShortcut(const QString &link, const QString &target, QString &error);
QString shortcutTarget(const QString &link, QString &error);
class RecycleSession {
    QSet<QString> checkedVolumes;
    int queries = 0;
    friend bool recycleFile(const QString &, QString &, QByteArray &, FileRecord &, QString &, RecycleSession *);
public:
    int queryCount() const { return queries; }
};
bool recycleFile(const QString &path, QString &recycledPath, QByteArray &itemId, FileRecord &binMetadata, QString &error,
                 RecycleSession *session = nullptr);
bool restoreRecycled(const QByteArray &itemId, const QString &recycledPath, const QString &destination, QString &error);
bool durableAppend(const QString &path, const QByteArray &line, QString &error);
class ComScope {
    HRESULT result;
public:
    ComScope(); ~ComScope(); bool valid() const { return SUCCEEDED(result); }
};
}
