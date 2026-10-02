#include "scanner.h"
#include "windowsfs.h"
#include <QDir>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QMap>
#include <QSet>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <winioctl.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace dup {
bool parseUsnBuffer(const QByteArray &buffer, QHash<quint64, MftEntry> &entries, bool changes, qint64 upper, QString &error) {
    if (buffer.size() < 8) { error = QStringLiteral("USN 缓冲区缺少起始位置。"); return false; }
    int offset = 8;
    while (offset < buffer.size()) {
        if (buffer.size() - offset < 8) { error = QStringLiteral("USN 记录截断。"); return false; }
        USN_RECORD record{};
        std::memcpy(&record, buffer.constData() + offset, 8);
        const int header = int(offsetof(USN_RECORD, FileName));
        if (record.MajorVersion != 2 || record.RecordLength < DWORD(header) || record.RecordLength > DWORD(buffer.size() - offset)) {
            error = QStringLiteral("USN 版本或记录长度不支持，切换目录遍历。"); return false;
        }
        std::memcpy(&record, buffer.constData() + offset, size_t(header));
        if (record.FileNameOffset < header || (record.FileNameOffset % 2) || (record.FileNameLength % 2) ||
            quint32(record.FileNameOffset) + record.FileNameLength > record.RecordLength) {
            error = QStringLiteral("USN 文件名范围无效。"); return false;
        }
        const QString name = QString::fromWCharArray(reinterpret_cast<const wchar_t *>(buffer.constData() + offset + record.FileNameOffset), record.FileNameLength / 2);
        const bool volumeRoot = name == "." && (record.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && record.FileReferenceNumber == record.ParentFileReferenceNumber;
        if (name.isEmpty() || name.contains('/') || name.contains('\\') || (name == "." && !volumeRoot) || name == "..") {
            error = QStringLiteral("USN 文件名无效。"); return false;
        }
        if (!changes || record.Usn < upper) {
            if (changes && record.Reason & USN_REASON_FILE_DELETE) entries.remove(record.FileReferenceNumber);
            else entries.insert(record.FileReferenceNumber, {record.FileReferenceNumber, record.ParentFileReferenceNumber, name, record.FileAttributes});
        }
        offset += int(record.RecordLength);
    }
    return true;
}
namespace {
bool cancelled(const Cancel &c) { return c && c->load(); }
struct Index { bool ready = false; QString reason; QHash<quint64, MftEntry> entries; };
bool queryJournal(HANDLE h, USN_JOURNAL_DATA &data) {
    DWORD bytes = 0;
    return DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &data, sizeof(data), &bytes, nullptr) && bytes >= sizeof(data);
}
Index buildIndex(const QString &volume, const Cancel &cancel, const Progress &progress) {
    Index result;
    const auto path = ("\\\\.\\" + volume.left(2)).toStdWString();
    win::Handle h(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, 0, nullptr));
    USN_JOURNAL_DATA before{};
    if (!h.valid() || !queryJournal(h.value, before)) {
        result.reason = QStringLiteral("卷或 USN 日志不可读（通常需要管理员权限）：") + win::errorText(); return result;
    }
    MFT_ENUM_DATA request{};
    request.HighUsn = std::numeric_limits<USN>::max();
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    while (!cancelled(cancel)) {
        DWORD bytes = 0;
        if (!DeviceIoControl(h.value, FSCTL_ENUM_USN_DATA, &request, sizeof(request), buffer.data(), DWORD(buffer.size()), &bytes, nullptr)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            result.reason = win::errorText(); return result;
        }
        if (bytes < 8) { result.reason = QStringLiteral("MFT 返回数据不足。"); return result; }
        quint64 next = 0; std::memcpy(&next, buffer.constData(), 8);
        if (next <= request.StartFileReferenceNumber || !parseUsnBuffer(QByteArray(buffer.constData(), int(bytes)), result.entries, false, 0, result.reason)) {
            if (result.reason.isEmpty()) result.reason = QStringLiteral("MFT 枚举未前进。");
            return result;
        }
        request.StartFileReferenceNumber = next;
        if (progress) progress(QStringLiteral("读取 NTFS 文件索引：") + volume, quint64(result.entries.size()), 0);
    }
    if (cancelled(cancel)) return result;
    USN_JOURNAL_DATA after{};
    if (!queryJournal(h.value, after) || before.UsnJournalID != after.UsnJournalID || before.NextUsn < after.FirstUsn) {
        result.reason = QStringLiteral("USN 日志变化或截断，无法校准索引。"); return result;
    }
    // Reconcile changes during enumeration to a bounded checkpoint, without modifying the journal.
    READ_USN_JOURNAL_DATA read{};
    read.StartUsn = before.NextUsn; read.ReasonMask = 0xffffffff; read.UsnJournalID = before.UsnJournalID;
    while (read.StartUsn < after.NextUsn && !cancelled(cancel)) {
        DWORD bytes = 0;
        if (!DeviceIoControl(h.value, FSCTL_READ_USN_JOURNAL, &read, sizeof(read), buffer.data(), DWORD(buffer.size()), &bytes, nullptr) || bytes < 8) {
            result.reason = QStringLiteral("读取 USN 增量失败：") + win::errorText(); return result;
        }
        USN next = 0; std::memcpy(&next, buffer.constData(), 8);
        if (next <= read.StartUsn || !parseUsnBuffer(QByteArray(buffer.constData(), int(bytes)), result.entries, true, after.NextUsn, result.reason)) {
            if (result.reason.isEmpty()) result.reason = QStringLiteral("USN 增量未前进。");
            return result;
        }
        read.StartUsn = next;
    }
    result.ready = !cancelled(cancel);
    return result;
}
quint64 directoryId(const QString &root) {
    win::Handle h(CreateFileW(win::nativePath(root).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!h.valid() || !GetFileInformationByHandle(h.value, &info)) return 0;
    return (quint64(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}
QString volumeFor(const QString &root) {
    wchar_t volume[MAX_PATH]{}, filesystem[64]{};
    const auto path = QDir::toNativeSeparators(root).toStdWString();
    if (!GetVolumePathNameW(path.c_str(), volume, MAX_PATH) ||
        !GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr, nullptr, filesystem, 64) ||
        QString::fromWCharArray(filesystem) != "NTFS") return {};
    const QString v = QDir::fromNativeSeparators(QString::fromWCharArray(volume));
    // Volume mount points and UNC paths use the native backend.
    return v.size() == 3 && v[1] == ':' ? v : QString();
}
void message(ScanResult &r, const QString &m) { if (r.messages.size() < 250) r.messages.append(m); }
void addFile(const QString &path, const QString &root, const QString &side, QVector<FileRecord> &files, ScanResult &result, const Progress &progress, const WIN32_FIND_DATAW *discovered = nullptr) {
    ++result.enumerated;
    FileRecord f; QString error;
    const QString name = discovered ? QString::fromWCharArray(discovered->cFileName) : QFileInfo(path).fileName();
    if (name.endsWith(".lnk", Qt::CaseInsensitive) || name.startsWith(".dupkiller-") || (result.options.photosOnly && !photoName(name))) ++result.skipped;
    else if (discovered) {
        constexpr DWORD excluded = FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE | 0x00040000 | 0x00400000;
        if (discovered->dwFileAttributes & excluded) ++result.skipped;
        else {
            // Directory enumeration already supplies name and size. Open only buckets that could match.
            f.path = path; f.root = root; f.side = side; f.name = name;
            f.size = (quint64(discovered->nFileSizeHigh) << 32) | discovered->nFileSizeLow;
            files.append(f);
        }
    } else {
        ++result.inspected;
        if (win::inspect(path, root, side, f, error)) files.append(f);
        else { ++result.skipped; message(result, path + "：" + error); }
    }
    if (progress && result.enumerated % 256 == 0) progress(QStringLiteral("枚举文件"), result.enumerated, 0);
}
void nativeEnumerate(const QString &root, const QString &side, QVector<FileRecord> &files, ScanResult &result, const Cancel &cancel, const Progress &progress) {
    QVector<QString> stack{root};
    while (!stack.isEmpty() && !cancelled(cancel)) {
        const QString dir = stack.takeLast();
        (side == "A" ? result.foldersA : result.foldersB).append(dir);
        WIN32_FIND_DATAW data{};
        const auto pattern = win::nativePath(dir + "/*");
        HANDLE search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (search == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER)
            search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
        if (search == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) { ++result.skipped; message(result, dir + "：" + win::errorText()); }
            continue;
        }
        do {
            if (cancelled(cancel)) break;
            const QString name = QString::fromWCharArray(data.cFileName);
            if (name == "." || name == "..") continue;
            const QString path = QDir(dir).filePath(name);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(data.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_OFFLINE))) stack.append(path);
                else ++result.skipped;
            } else addFile(path, root, side, files, result, progress, &data);
        } while (FindNextFileW(search, &data));
        const DWORD error = GetLastError();
        FindClose(search);
        if (!cancelled(cancel) && error != ERROR_NO_MORE_FILES) message(result, dir + "：" + win::errorText(error));
    }
}
bool indexedEnumerate(const Index &index, const QString &root, const QString &side, QVector<FileRecord> &files, ScanResult &result, const Cancel &cancel, const Progress &progress) {
    const quint64 rootId = directoryId(root);
    if (!rootId || !index.entries.contains(rootId)) return false;
    QHash<quint64, QString> parents;
    QSet<quint64> outside;
    parents.insert(rootId, root);
    auto resolve = [&](quint64 id) {
        QVector<quint64> chain; QSet<quint64> seen;
        quint64 current = id;
        while (!parents.contains(current) && !outside.contains(current)) {
            const auto it = index.entries.constFind(current);
            if (it == index.entries.constEnd() || seen.contains(current) || !(it->attributes & FILE_ATTRIBUTE_DIRECTORY) ||
                it->attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_OFFLINE)) break;
            chain.append(current); seen.insert(current); current = it->parent;
        }
        if (!parents.contains(current)) { for (auto node : chain) outside.insert(node); outside.insert(id); return QString(); }
        QString path = parents.value(current);
        for (int i = chain.size() - 1; i >= 0; --i) { path = QDir(path).filePath(index.entries.value(chain[i]).name); parents.insert(chain[i], path); }
        return path;
    };
    for (auto it = index.entries.constBegin(); it != index.entries.constEnd() && !cancelled(cancel); ++it) {
        if (it->attributes & FILE_ATTRIBUTE_DIRECTORY) {
            const auto directory = resolve(it.key());
            if (!directory.isEmpty()) (side == "A" ? result.foldersA : result.foldersB).append(directory);
            continue;
        }
        const QString parent = resolve(it->parent);
        if (!parent.isEmpty()) addFile(QDir(parent).filePath(it->name), root, side, files, result, progress);
    }
    return true;
}
}
ScanResult Scanner::scan(ScanOptions options, const Cancel &cancel, const Progress &progress) {
    ScanResult result;
    result.error = validateRoots(options); result.options = options;
    if (!result.error.isEmpty()) return result;
    QElapsedTimer timer; timer.start();
    const int recommended = qMin(win::recommendedHashWorkers(options.rootA), options.mode == Mode::Dual ? win::recommendedHashWorkers(options.rootB) : 4);
    result.hashWorkers = options.hashWorkers > 0 ? qBound(1, options.hashWorkers, 4) : qMin(recommended, qMax(1, QThread::idealThreadCount()));
    message(result, QStringLiteral("磁盘读取并发：%1；机械盘或无法确认的设备采用单路读取。").arg(result.hashWorkers));
    QVector<FileRecord> files;
    QHash<QString, Index> indexes;
    auto enumerate = [&](const QString &root, const QString &side) {
        const QString volume = options.backend == Backend::Auto && recommended > 1 ? volumeFor(root) : QString();
        if (!volume.isEmpty()) {
            if (!indexes.contains(volume)) indexes.insert(volume, buildIndex(volume, cancel, progress));
            const auto &index = indexes[volume];
            if (cancelled(cancel)) return;
            if (index.ready && indexedEnumerate(index, root, side, files, result, cancel, progress)) {
                message(result, root + QStringLiteral("：使用 NTFS MFT/USN 加速（已校准增量）。")); return;
            }
            message(result, root + QStringLiteral("：加速不可用，使用原生遍历。") + index.reason);
        }
        nativeEnumerate(root, side, files, result, cancel, progress);
        message(result, root + QStringLiteral("：Windows 原生目录遍历。"));
    };
    enumerate(options.rootA, "A");
    if (options.mode == Mode::Dual && !cancelled(cancel)) enumerate(options.rootB, "B");
    indexes.clear();
    result.enumerateMs = timer.elapsed(); timer.restart();
    if (cancelled(cancel)) { result.cancelled = true; return result; }
    std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
    QHash<QString, QMap<quint64, QVector<int>>> buckets;
    for (int i = 0; i < files.size(); ++i) buckets[files[i].name][files[i].size].append(i);
    std::vector<FileRecord> candidates;
    for (const auto &sizes : buckets) for (const auto &indices : sizes) {
        if (indices.size() < 2) continue;
        bool a = false, b = false;
        for (int i : indices) { a |= files[i].side == "A"; b |= files[i].side == "B"; }
        if (options.mode == Mode::Dual && !(a && b)) continue;
        for (int i : indices) candidates.push_back(files[i]);
    }
    files.clear(); buckets.clear();
    std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
    // A prefix is only a rejection filter. Every surviving match still receives full SHA-256 and stream checks.
    QHash<QString, QVector<size_t>> sampledBuckets;
    std::vector<FileRecord> filtered;
    for (size_t i = 0; i < candidates.size() && !cancelled(cancel); ++i) {
        auto &f = candidates[i]; QString error; bool ok = true;
        if (!f.fileId) {
            const auto expectedSize = f.size; ++result.inspected;
            ok = win::inspect(f.path, f.root, f.side, f, error) && f.size == expectedSize;
            if (!ok && error.isEmpty()) error = QStringLiteral("文件大小在枚举后变化。");
        }
        if (!ok) { ++result.skipped; message(result, f.path + "：" + error); continue; }
        if (options.sampling && f.size > 128 * 1024) {
            QByteArray digest; ++result.sampled;
            if (!win::sampleFile(f, digest, cancel, error)) { ++result.skipped; message(result, f.path + "：" + error); continue; }
            const QString key = f.name + QChar(0) + QString::number(f.size) + QChar(0) + QString::fromLatin1(digest.toHex());
            sampledBuckets[key].append(i);
        } else filtered.push_back(f);
        if (progress && i % 64 == 0) progress(QStringLiteral("检查候选元数据及文件头"), i + 1, candidates.size());
    }
    for (const auto &indices : sampledBuckets) {
        bool a = false, b = false;
        for (size_t i : indices) { a |= candidates[i].side == "A"; b |= candidates[i].side == "B"; }
        if (indices.size() < 2 || (options.mode == Mode::Dual && !(a && b))) { result.sampleRejected += indices.size(); continue; }
        for (size_t i : indices) filtered.push_back(std::move(candidates[i]));
    }
    candidates = std::move(filtered); sampledBuckets.clear();
    std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
    for (const auto &f : candidates) result.hashBytes += f.size;
    result.sampleMs = timer.elapsed(); timer.restart();
    if (cancelled(cancel)) { result.cancelled = true; return result; }
    struct Hashed { FileRecord file; QString error; bool ok = false; };
    std::vector<Hashed> hashed(candidates.size());
    std::atomic_size_t next{0}; std::atomic<quint64> readBytes{0};
    QThreadPool pool;
    pool.setMaxThreadCount(result.hashWorkers);
    QVector<QFuture<void>> jobs;
    if (progress) progress(QStringLiteral("SHA-256 数据读取（字节）"), 0, result.hashBytes);
    for (int worker = 0; worker < pool.maxThreadCount(); ++worker) jobs.append(QtConcurrent::run(&pool, [&] {
        for (;;) {
            if (cancelled(cancel)) break;
            const size_t i = next.fetch_add(1);
            if (i >= candidates.size()) break;
            auto &out = hashed[i]; out.file = candidates[i];
            out.ok = win::hashFile(out.file, cancel, out.error, [&](quint64 bytes) {
                const auto done = readBytes.fetch_add(bytes) + bytes;
                if (progress) progress(QStringLiteral("SHA-256 数据读取（字节）"), done, result.hashBytes);
            });
        }
    }));
    for (auto &job : jobs) job.waitForFinished();
    result.hashMs = timer.elapsed();
    if (cancelled(cancel)) { result.cancelled = true; return result; }
    QMap<QString, DuplicateGroup> groups;
    for (const auto &item : hashed) {
        if (!item.ok) { ++result.skipped; message(result, item.file.path + "：" + item.error); continue; }
        ++result.hashed;
        const auto &f = item.file;
        QString key = f.name + QChar(0) + QString::number(f.size) + QChar(0) + QString::fromLatin1(f.sha256.toHex());
        for (auto it = f.streams.constBegin(); it != f.streams.constEnd(); ++it)
            key += QChar(0) + it.key() + QChar(0) + QString::fromLatin1(it.value().toHex());
        groups[key].files.append(f);
    }
    for (auto g : groups) {
        if (g.files.size() < 2) continue;
        bool a = false, b = false;
        for (const auto &f : g.files) { a |= f.side == "A"; b |= f.side == "B"; }
        if (options.mode == Mode::Dual && !(a && b)) continue;
        setKeeper(g, options.mode, pickKeeper(g, options.mode, KeepRule::Shallowest));
        result.groups.append(g);
    }
    if (progress) progress(QStringLiteral("扫描完成"), result.enumerated, result.enumerated);
    return result;
}
}
