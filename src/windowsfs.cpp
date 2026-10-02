#include "windowsfs.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <QTimeZone>
#include <cstring>

namespace dup::win {
namespace {
template<class T> struct ComPtr {
    T *p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T *operator->() const { return p; }
};
constexpr DWORD excludedAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_SYSTEM |
    FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE | 0x00040000 | 0x00400000;
quint64 ticks(FILETIME ft) { return (quint64(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }
QString hrError(HRESULT hr) { return QStringLiteral("Windows Shell 错误 0x%1").arg(quint32(hr), 8, 16, QLatin1Char('0')); }
class RecycleSink final : public IFileOperationProgressSink {
    LONG refs = 1;
public:
    HRESULT deletion = E_FAIL;
    QString destination;
    QByteArray itemId;
    bool recycled = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, IID_IFileOperationProgressSink)) { *out = static_cast<IFileOperationProgressSink *>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { const LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem *, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem *, IShellItem *, LPCWSTR, HRESULT, IShellItem *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags, IShellItem *) override {
        // Refuse a Shell operation that is not explicitly a recycle operation.
        return flags & TSF_DELETE_RECYCLE_IF_POSSIBLE ? S_OK : E_ABORT;
    }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem *, HRESULT hr, IShellItem *created) override {
        deletion = hr; recycled = SUCCEEDED(hr) && created;
        if (created) {
            PWSTR name = nullptr;
            if (SUCCEEDED(created->GetDisplayName(SIGDN_FILESYSPATH, &name))) { destination = QString::fromWCharArray(name); CoTaskMemFree(name); }
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHGetIDListFromObject(created, &pidl))) {
                itemId = QByteArray(reinterpret_cast<const char *>(pidl), int(ILGetSize(pidl))); CoTaskMemFree(pidl);
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem *, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem *, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
};
}
ComScope::ComScope() : result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
ComScope::~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
QString errorText(DWORD code) {
    wchar_t *message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t *>(&message), 0, nullptr);
    const QString result = message ? QString::fromWCharArray(message).trimmed() : QString::number(code);
    if (message) LocalFree(message);
    return result + QStringLiteral(" (%1)").arg(code);
}
std::wstring nativePath(const QString &p) {
    QString s = QDir::toNativeSeparators(cleanPath(p));
    if (!s.startsWith("\\\\?\\")) s = s.startsWith("\\\\") ? "\\\\?\\UNC\\" + s.mid(2) : "\\\\?\\" + s;
    return s.toStdWString();
}
bool lockParents(const QString &file, std::vector<Handle> &locks, QString &error) {
    QString dir = QFileInfo(cleanPath(file)).absolutePath();
    QStringList parents;
    for (;;) {
        parents.prepend(dir);
        const QString next = QFileInfo(dir).absolutePath();
        if (next == dir || dir.endsWith(":/")) break;
        dir = next;
    }
    // A non-delete-sharing handle on the immediate parent also prevents Windows
    // from renaming its ancestors. Do not require handles on unrelated ancestors.
    const QString immediate = parents.last();
    Handle h(CreateFileW(nativePath(immediate).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!h.valid()) { error = immediate + QStringLiteral("：") + errorText(); return false; }
    for (const auto &parent : parents) {
        const DWORD attributes = GetFileAttributesW(nativePath(parent).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) { error = parent + QStringLiteral("：") + errorText(); return false; }
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            error = QStringLiteral("路径包含符号链接、目录联接或无效目录：") + parent; return false;
        }
    }
    locks.push_back(std::move(h));
    return true;
}
bool safeDirectory(const QString &path, QString &error) {
    if (!QFileInfo(path).isDir()) { error = QStringLiteral("目录不存在或不可访问。"); return false; }
    std::vector<Handle> locks;
    return lockParents(path + "/__dup_validation__", locks, error);
}
bool metadata(HANDLE h, FileRecord &f, QString &error, bool allowSystem) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info)) { error = errorText(); return false; }
    const DWORD excluded = allowSystem ? excludedAttributes & ~FILE_ATTRIBUTE_SYSTEM : excludedAttributes;
    if (info.dwFileAttributes & excluded || info.nNumberOfLinks != 1) {
        error = QStringLiteral("跳过系统/链接/云端未下载文件或已有硬链接。"); return false;
    }
    f.fileId = (quint64(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    f.volumeId = info.dwVolumeSerialNumber;
    f.size = (quint64(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    f.writeTicks = ticks(info.ftLastWriteTime);
    f.attributes = info.dwFileAttributes;
    f.modified = QDateTime::fromMSecsSinceEpoch(qint64(f.writeTicks / 10000) - 11644473600000LL, QTimeZone::utc()).toLocalTime();
    return true;
}
bool hasExtraStreams(const QString &path) {
    WIN32_FIND_STREAM_DATA data{};
    HANDLE h = FindFirstStreamW(nativePath(path).c_str(), FindStreamInfoStandard, &data, 0);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        return e != ERROR_HANDLE_EOF && e != ERROR_INVALID_PARAMETER && e != ERROR_NOT_SUPPORTED;
    }
    bool extra = false;
    do { if (QString::fromWCharArray(data.cStreamName) != "::$DATA") { extra = true; break; } } while (FindNextStreamW(h, &data));
    const DWORD e = GetLastError();
    FindClose(h);
    return extra || e != ERROR_HANDLE_EOF;
}
namespace {
bool streamNames(const QString &path, QMap<QString, quint64> &names, QString &error) {
    WIN32_FIND_STREAM_DATA data{};
    HANDLE find = FindFirstStreamW(nativePath(path).c_str(), FindStreamInfoStandard, &data, 0);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_HANDLE_EOF || code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED) return true;
        error = errorText(code); return false;
    }
    do {
        const QString name = QString::fromWCharArray(data.cStreamName);
        if (name == "::$DATA") continue;
        if (!name.startsWith(':') || !name.endsWith(":$DATA") || name.contains('/') || name.contains('\\') || name.size() <= 7) {
            FindClose(find); error = QStringLiteral("不支持的数据流名称。"); return false;
        }
        names.insert(name, quint64(data.StreamSize.QuadPart));
    } while (FindNextStreamW(find, &data));
    const DWORD code = GetLastError(); FindClose(find);
    if (code != ERROR_HANDLE_EOF) { error = errorText(code); return false; }
    return true;
}
}
bool hashStreams(const QString &path, QMap<QString, QByteArray> &hashes, const Cancel &cancel, QString &error,
                 std::vector<Handle> *guards) {
    QMap<QString, quint64> before, after; hashes.clear();
    if (!streamNames(path, before, error)) return false;
    std::vector<Handle> locked;
    for (auto it = before.constBegin(); it != before.constEnd(); ++it) {
        // Named streams must allow the transaction's existing DELETE handle,
        // while denying writes until the corresponding transaction ends.
        Handle stream(CreateFileW(nativePath(path + it.key()).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        LARGE_INTEGER size{}; QByteArray digest;
        if (!stream.valid() || !GetFileSizeEx(stream.value, &size) || quint64(size.QuadPart) != it.value() ||
            !hashHandle(stream.value, digest, cancel, error)) {
            if (error.isEmpty()) error = QStringLiteral("备用数据流不可读取或大小已变化：") + it.key();
            return false;
        }
        hashes.insert(it.key(), digest); locked.push_back(std::move(stream));
    }
    if (!streamNames(path, after, error) || before != after) { if (error.isEmpty()) error = QStringLiteral("备用数据流在校验期间变化。"); return false; }
    if (guards) for (auto &handle : locked) guards->push_back(std::move(handle));
    return true;
}
bool copyStreams(const QString &source, const QString &destination, const QMap<QString, QByteArray> &expected,
                 const Cancel &cancel, QString &error) {
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it) {
        Handle input(CreateFileW(nativePath(source + it.key()).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        Handle output(CreateFileW(nativePath(destination + it.key()).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  nullptr, CREATE_NEW, 0, nullptr));
        if (!input.valid() || !output.valid()) { error = errorText(); return false; }
        QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        for (;;) {
            if (cancel && cancel->load()) { error = QStringLiteral("已取消。"); return false; }
            DWORD read = 0, written = 0;
            if (!ReadFile(input.value, buffer.data(), DWORD(buffer.size()), &read, nullptr) ||
                (read && (!WriteFile(output.value, buffer.constData(), read, &written, nullptr) || written != read))) {
                error = errorText(); return false;
            }
            if (!read) break;
        }
        if (!FlushFileBuffers(output.value)) { error = errorText(); return false; }
    }
    QMap<QString, QByteArray> copied;
    if (!hashStreams(destination, copied, cancel, error)) return false;
    if (copied != expected) { error = QStringLiteral("复制的备用数据流未通过 SHA-256 校验。"); return false; }
    return true;
}
bool inspect(const QString &path, const QString &root, const QString &side, FileRecord &f, QString &error) {
    f.path = cleanPath(path); f.root = root; f.side = side; f.name = QFileInfo(path).fileName();
    if (f.name.endsWith(".lnk", Qt::CaseInsensitive) || f.name.startsWith(".dupkiller-")) { error = QStringLiteral("跳过快捷方式或事务暂存文件。"); return false; }
    Handle h(CreateFileW(nativePath(path).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!h.valid()) { error = errorText(); return false; }
    if (!metadata(h.value, f, error)) return false;
    f.depth = QDir(root).relativeFilePath(f.path).count('/');
    return true;
}
bool unchanged(const FileRecord &a, const FileRecord &b) {
    return a.fileId == b.fileId && a.volumeId == b.volumeId && a.size == b.size && a.writeTicks == b.writeTicks && a.attributes == b.attributes;
}
bool hashHandle(HANDLE h, QByteArray &hash, const Cancel &cancel, QString &error) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) { error = errorText(); return false; }
    QCryptographicHash digest(QCryptographicHash::Sha256);
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        if (cancel && cancel->load()) { error = QStringLiteral("已取消。"); return false; }
        DWORD bytes = 0;
        if (!ReadFile(h, buffer.data(), DWORD(buffer.size()), &bytes, nullptr)) { error = errorText(); return false; }
        if (!bytes) break;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        digest.addData(QByteArrayView(buffer.constData(), int(bytes)));
#else
        digest.addData(buffer.constData(), int(bytes));
#endif
    }
    hash = digest.result();
    return true;
}
bool hashFile(FileRecord &f, const Cancel &cancel, QString &error) {
    std::vector<Handle> parents;
    if (!lockParents(f.path, parents, error)) return false;
    Handle h(CreateFileW(nativePath(f.path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!h.valid()) { error = errorText(); return false; }
    FileRecord current;
    if (!metadata(h.value, current, error)) return false;
    if (!unchanged(f, current)) { error = QStringLiteral("文件在扫描期间发生变化。"); return false; }
    if (!hashHandle(h.value, f.sha256, cancel, error)) return false;
    if (!hashStreams(f.path, f.streams, cancel, error)) return false;
    FileRecord after;
    return metadata(h.value, after, error) && unchanged(current, after);
}
bool renameHandle(HANDLE h, const QString &destination, QString &error) {
    const auto dest = nativePath(destination);
    const DWORD length = DWORD(dest.size() * sizeof(wchar_t));
    // Win32's DOS-path translation may inspect the terminating NUL even though
    // FileNameLength counts only the name bytes.
    std::vector<unsigned char> storage(offsetof(FILE_RENAME_INFO, FileName) + length + sizeof(wchar_t), 0);
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    rename->ReplaceIfExists = FALSE; rename->RootDirectory = nullptr; rename->FileNameLength = length;
    std::memcpy(rename->FileName, dest.data(), length);
    if (!SetFileInformationByHandle(h, FileRenameInfo, rename, DWORD(storage.size()))) { error = errorText(); return false; }
    return true;
}
bool createShortcut(const QString &link, const QString &target, QString &error) {
    ComPtr<IShellLinkW> shell;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void **>(&shell.p));
    const auto t = QDir::toNativeSeparators(target).toStdWString(), l = QDir::toNativeSeparators(link).toStdWString();
    const auto working = QDir::toNativeSeparators(QFileInfo(target).absolutePath()).toStdWString();
    if (SUCCEEDED(hr)) hr = shell->SetPath(t.c_str());
    if (SUCCEEDED(hr)) hr = shell->SetWorkingDirectory(working.c_str());
    if (SUCCEEDED(hr)) hr = shell->SetDescription(L"QtDupKiller：重复文件的原始副本");
    ComPtr<IPersistFile> persist;
    if (SUCCEEDED(hr)) hr = shell->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&persist.p));
    if (SUCCEEDED(hr)) hr = persist->Save(l.c_str(), TRUE);
    if (FAILED(hr)) { error = hrError(hr); return false; }
    Handle h(CreateFileW(nativePath(link).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!h.valid() || !FlushFileBuffers(h.value)) { error = errorText(); return false; }
    return true;
}
QString shortcutTarget(const QString &link, QString &error) {
    ComPtr<IShellLinkW> shell;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void **>(&shell.p));
    ComPtr<IPersistFile> persist;
    if (SUCCEEDED(hr)) hr = shell->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&persist.p));
    const auto l = QDir::toNativeSeparators(link).toStdWString();
    if (SUCCEEDED(hr)) hr = persist->Load(l.c_str(), STGM_READ);
    wchar_t buffer[32768]{};
    if (SUCCEEDED(hr)) hr = shell->GetPath(buffer, 32768, nullptr, SLGP_RAWPATH);
    if (FAILED(hr)) { error = hrError(hr); return {}; }
    return cleanPath(QString::fromWCharArray(buffer));
}
bool recycleFile(const QString &path, QString &recycledPath, QByteArray &itemId, FileRecord &binMetadata, QString &error) {
    wchar_t volume[MAX_PATH]{};
    const auto p = QDir::toNativeSeparators(path).toStdWString();
    if (!GetVolumePathNameW(p.c_str(), volume, MAX_PATH) || GetDriveTypeW(volume) != DRIVE_FIXED) {
        error = QStringLiteral("该位置不支持本程序的安全回收站操作；请明确选择永久删除或跳过。"); return false;
    }
    SHQUERYRBINFO bin{}; bin.cbSize = sizeof(bin);
    const HRESULT query = SHQueryRecycleBinW(volume, &bin);
    if (FAILED(query)) { error = QStringLiteral("回收站不可用，未永久删除：") + hrError(query); return false; }
    ComPtr<IFileOperation> operation;
    ComPtr<IShellItem> item;
    ComPtr<RecycleSink> sink; sink.p = new RecycleSink;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOperation, reinterpret_cast<void **>(&operation.p));
    if (SUCCEEDED(hr)) hr = operation->SetOperationFlags(FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD | FOFX_EARLYFAILURE |
        FOF_ALLOWUNDO | FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING);
    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(p.c_str(), nullptr, IID_IShellItem, reinterpret_cast<void **>(&item.p));
    if (SUCCEEDED(hr)) hr = operation->DeleteItem(item.p, sink.p);
    if (SUCCEEDED(hr)) hr = operation->PerformOperations();
    BOOL aborted = TRUE;
    if (operation.p) operation->GetAnyOperationsAborted(&aborted);
    if (FAILED(hr) || aborted || FAILED(sink->deletion) || !sink->recycled) {
        error = hrError(FAILED(hr) ? hr : sink->deletion) + QStringLiteral("；回收站操作未确认成功。"); return false;
    }
    recycledPath = sink->destination;
    itemId = sink->itemId;
    const QString name = QFileInfo(recycledPath).fileName();
    if (name.startsWith("$R") && cleanPath(recycledPath).contains("/$Recycle.Bin/", Qt::CaseInsensitive)) {
        binMetadata.path = cleanPath(QFileInfo(recycledPath).absolutePath() + "/$I" + name.mid(2));
        Handle descriptor(CreateFileW(nativePath(binMetadata.path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        QString detail;
        if (!descriptor.valid() || !metadata(descriptor.value, binMetadata, detail, true) || !hashHandle(descriptor.value, binMetadata.sha256, {}, detail)) {
            binMetadata = {}; error = QStringLiteral("回收成功，但无法记录回收站索引身份：") + detail;
        }
    }
    return true;
}
bool restoreRecycled(const QByteArray &itemId, const QString &recycledPath, const QString &destination, QString &error) {
    // Validate serialized ITEMIDLIST boundaries before passing it to the Shell.
    int offset = 0; bool terminated = false;
    if (itemId.size() > 65536) { error = QStringLiteral("回收站项目标识过长。"); return false; }
    while (offset + 2 <= itemId.size()) {
        USHORT size = 0; std::memcpy(&size, itemId.constData() + offset, 2);
        if (size == 0) { terminated = offset + 2 == itemId.size(); break; }
        if (size < 2 || offset + size > itemId.size()) break;
        offset += size;
    }
    if (!terminated) { error = QStringLiteral("回收站项目标识无效，请从系统回收站检查。"); return false; }
    if (GetFileAttributesW(nativePath(destination).c_str()) != INVALID_FILE_ATTRIBUTES) { error = QStringLiteral("原路径已有文件，未覆盖。"); return false; }
    ComPtr<IShellItem> item, folder;
    ComPtr<IFileOperation> operation;
    HRESULT hr = SHCreateItemFromIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(itemId.constData()), IID_IShellItem, reinterpret_cast<void **>(&item.p));
    PWSTR name = nullptr;
    if (SUCCEEDED(hr)) hr = item->GetDisplayName(SIGDN_FILESYSPATH, &name);
    if (SUCCEEDED(hr)) {
        const bool same = samePath(QString::fromWCharArray(name), recycledPath); CoTaskMemFree(name);
        if (!same) { error = QStringLiteral("回收站标识与日志路径不匹配。"); return false; }
    }
    const auto parent = QDir::toNativeSeparators(QFileInfo(destination).absolutePath()).toStdWString();
    const auto filename = QFileInfo(destination).fileName().toStdWString();
    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(parent.c_str(), nullptr, IID_IShellItem, reinterpret_cast<void **>(&folder.p));
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOperation, reinterpret_cast<void **>(&operation.p));
    if (SUCCEEDED(hr)) hr = operation->SetOperationFlags(FOF_SILENT | FOF_NOERRORUI | FOFX_EARLYFAILURE | FOF_NOCONFIRMMKDIR);
    if (SUCCEEDED(hr)) hr = operation->MoveItem(item.p, folder.p, filename.c_str(), nullptr);
    if (SUCCEEDED(hr)) hr = operation->PerformOperations();
    BOOL aborted = TRUE;
    if (operation.p) operation->GetAnyOperationsAborted(&aborted);
    if (FAILED(hr) || aborted) { error = hrError(hr); return false; }
    return true;
}
bool durableAppend(const QString &path, const QByteArray &line, QString &error) {
    Handle h(CreateFileW(nativePath(path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (!h.valid()) { error = errorText(); return false; }
    DWORD written = 0;
    if (!WriteFile(h.value, line.constData(), DWORD(line.size()), &written, nullptr) || written != DWORD(line.size()) || !FlushFileBuffers(h.value)) {
        error = errorText(); return false;
    }
    return true;
}
}
