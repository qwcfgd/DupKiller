#include "executor.h"
#include "windowsfs.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QUuid>

namespace dup {
namespace {
QJsonObject jsonFile(const FileRecord &f) {
    QJsonObject streams;
    for (auto it = f.streams.constBegin(); it != f.streams.constEnd(); ++it) streams[it.key()] = QString::fromLatin1(it.value().toHex());
    return {{"path", f.path}, {"root", f.root}, {"name", f.name}, {"side", f.side},
        {"size", QString::number(f.size)}, {"fileId", QString::number(f.fileId)}, {"volume", QString::number(f.volumeId)},
        {"writeTicks", QString::number(f.writeTicks)}, {"attributes", QString::number(f.attributes)}, {"sha256", QString::fromLatin1(f.sha256.toHex())}, {"streams", streams}};
}
FileRecord fromJson(const QJsonObject &o) {
    FileRecord f;
    f.path = o["path"].toString(); f.root = o["root"].toString(); f.name = o["name"].toString(); f.side = o["side"].toString();
    f.size = o["size"].toString().toULongLong(); f.fileId = o["fileId"].toString().toULongLong();
    f.volumeId = o["volume"].toString().toULongLong(); f.writeTicks = o["writeTicks"].toString().toULongLong();
    f.attributes = o["attributes"].toString().toUInt(); f.sha256 = QByteArray::fromHex(o["sha256"].toString().toLatin1());
    const auto streams = o["streams"].toObject();
    for (auto it = streams.constBegin(); it != streams.constEnd(); ++it) f.streams[it.key()] = QByteArray::fromHex(it.value().toString().toLatin1());
    return f;
}
bool record(const QString &log, QJsonObject &entry, const QString &phase, QString &error) {
    entry["phase"] = phase; entry["timeUtc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    return win::durableAppend(log, QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n', error);
}
win::Handle openFile(const QString &path, bool deletion, DWORD share = FILE_SHARE_READ) {
    return win::Handle(CreateFileW(win::nativePath(path).c_str(), GENERIC_READ | (deletion ? DELETE : 0), share, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
}
bool verify(HANDLE h, const FileRecord &expected, const QString &path, const Cancel &cancel, QString &error,
            std::vector<win::Handle> *streamGuards = nullptr) {
    FileRecord current; QByteArray hash;
    if (!win::metadata(h, current, error)) return false;
    if (!win::unchanged(expected, current)) { error = QStringLiteral("文件身份或属性已变化。"); return false; }
    if (!win::hashHandle(h, hash, cancel, error)) return false;
    if (hash != expected.sha256 || expected.sha256.size() != 32) { error = QStringLiteral("SHA-256 已变化，跳过替换。"); return false; }
    QMap<QString, QByteArray> streams;
    if (!win::hashStreams(path, streams, cancel, error, streamGuards) || streams != expected.streams) {
        if (error.isEmpty()) error = QStringLiteral("备用数据流已变化，跳过替换。");
        return false;
    }
    return true;
}
bool removeOwnedLink(const QString &link, const FileRecord &identity, QString &error) {
    win::Handle h = openFile(link, true);
    FileRecord now; QByteArray hash;
    QMap<QString, QByteArray> streams;
    if (!h.valid() || !win::metadata(h.value, now, error) || !win::unchanged(identity, now) || identity.sha256.size() != 32 ||
        !win::hashHandle(h.value, hash, {}, error) || hash != identity.sha256 ||
        !win::hashStreams(link, streams, {}, error) || streams != identity.streams) {
        error = QStringLiteral("快捷方式已变化，未移除：") + link; return false;
    }
    FILE_DISPOSITION_INFO disposition{}; disposition.DeleteFile = TRUE;
    if (!SetFileInformationByHandle(h.value, FileDispositionInfo, &disposition, sizeof(disposition))) { error = win::errorText(); return false; }
    return true;
}
bool replaceOne(const Operation &op, bool permanent, const QString &log, const Cancel &cancel, QString &error) {
    std::vector<win::Handle> parents;
    if (!win::lockParents(op.source.path, parents, error) || !win::lockParents(op.target.path, parents, error)) return false;
    win::Handle target = openFile(op.target.path, false), source = openFile(op.source.path, true);
    if (!target.valid() || !source.valid()) { error = win::errorText(); return false; }
    std::vector<win::Handle> sourceStreams, targetStreams;
    if (!verify(target.value, op.target, op.target.path, cancel, error, &targetStreams) ||
        !verify(source.value, op.source, op.source.path, cancel, error, &sourceStreams)) return false;
    if (op.source.attributes & FILE_ATTRIBUTE_READONLY) { error = QStringLiteral("副本为只读文件，未更改只读属性。"); return false; }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString staged = QFileInfo(op.source.path).absolutePath() + "/.dupkiller-" + id + ".pending";
    const QString tempLink = QFileInfo(op.source.path).absolutePath() + "/.dupkiller-" + id + ".lnk";
    const QString link = op.source.path + ".lnk";
    if (GetFileAttributesW(win::nativePath(link).c_str()) != INVALID_FILE_ATTRIBUTES) { error = QStringLiteral("同名快捷方式已存在，未覆盖。"); return false; }
    QJsonObject entry{{"id", id}, {"source", jsonFile(op.source)}, {"target", jsonFile(op.target)},
                      {"staged", staged}, {"link", link}, {"tempLink", tempLink}, {"permanent", permanent}};
    if (!record(log, entry, "prepared", error)) return false;
    auto removeTemp = [&] { DeleteFileW(win::nativePath(tempLink).c_str()); };
    if (!win::createShortcut(tempLink, op.target.path, error) || !samePath(win::shortcutTarget(tempLink, error), op.target.path) ||
        !win::copyStreams(op.source.path, tempLink, op.source.streams, cancel, error)) {
        removeTemp(); if (error.isEmpty()) error = QStringLiteral("快捷方式目标验证失败。"); return false;
    }
    FileRecord expectedLink;
    {
        win::Handle tempGuard = openFile(tempLink, false);
        if (!tempGuard.valid() || !win::metadata(tempGuard.value, expectedLink, error) || !win::hashHandle(tempGuard.value, expectedLink.sha256, {}, error) ||
            !win::hashStreams(tempLink, expectedLink.streams, {}, error)) {
            tempGuard.reset(); removeTemp(); if (error.isEmpty()) error = win::errorText(); return false;
        }
    }
    if (cancel && cancel->load()) { removeTemp(); error = QStringLiteral("已取消。"); return false; }
    if (!MoveFileExW(win::nativePath(tempLink).c_str(), win::nativePath(link).c_str(), MOVEFILE_WRITE_THROUGH)) {
        error = win::errorText(); removeTemp(); return false;
    }
    win::Handle linkGuard = openFile(link, false);
    FileRecord linkIdentity; linkIdentity.path = link;
    if (!linkGuard.valid() || !win::metadata(linkGuard.value, linkIdentity, error) || !win::unchanged(expectedLink, linkIdentity) ||
        !win::hashHandle(linkGuard.value, linkIdentity.sha256, {}, error) || linkIdentity.sha256 != expectedLink.sha256 ||
        !win::hashStreams(link, linkIdentity.streams, {}, error) || linkIdentity.streams != expectedLink.streams) {
        error = QStringLiteral("无法锁定已创建的快捷方式，原文件未删除；请检查：") + link; return false;
    }
    entry["linkIdentity"] = jsonFile(linkIdentity);
    bool stagedNow = false;
    auto rollback = [&] {
        QString detail;
        bool restored = true;
        if (stagedNow) {
            if (!source.valid()) source = openFile(staged, true);
            FileRecord current;
            restored = source.valid() && win::metadata(source.value, current, detail) && win::unchanged(current, op.source) &&
                       win::renameHandle(source.value, op.source.path, detail);
        }
        linkGuard.reset();
        if (restored) {
            if (!removeOwnedLink(link, linkIdentity, detail)) error += "；" + detail;
            record(log, entry, "rolled_back", detail);
        } else {
            error += QStringLiteral("；原文件仍在暂存位置，请使用日志恢复：") + staged + "；" + detail;
            record(log, entry, "recovery_required", detail);
        }
    };
    if (!record(log, entry, "link_created", error) || !win::renameHandle(source.value, staged, error)) { rollback(); return false; }
    stagedNow = true;
    if (!record(log, entry, "staged", error)) { rollback(); return false; }
    bool removed = false;
    if (permanent) {
        FILE_DISPOSITION_INFO disposition{}; disposition.DeleteFile = TRUE;
        removed = SetFileInformationByHandle(source.value, FileDispositionInfo, &disposition, sizeof(disposition));
        if (!removed) error = win::errorText();
        else source.reset();
    } else {
        source.reset();
        source = openFile(staged, true, FILE_SHARE_READ | FILE_SHARE_DELETE);
        if (!source.valid() || !verify(source.value, op.source, staged, {}, error)) {
            if (error.isEmpty()) error = win::errorText();
        } else {
            QString recycledPath; QByteArray itemId; FileRecord binMetadata;
            removed = win::recycleFile(staged, recycledPath, itemId, binMetadata, error);
            if (removed) {
                entry["recycledPath"] = recycledPath; entry["recycledItemId"] = QString::fromLatin1(itemId.toBase64());
                if (!binMetadata.path.isEmpty()) entry["recycleMetadata"] = jsonFile(binMetadata);
            }
        }
        source.reset();
    }
    if (!removed) { rollback(); return false; }
    QString journalError;
    if (!record(log, entry, "committed", journalError)) error = QStringLiteral("替换成功，但完成日志写入失败：") + journalError;
    return true;
}
QString validatePlan(const QVector<Operation> &plan, ScanOptions options) {
    const QString roots = validateRoots(options);
    if (!roots.isEmpty()) return roots;
    QSet<QString> sources;
    QSet<QString> targets;
    for (const auto &op : plan) {
        const QString root = options.mode == Mode::Dual ? options.rootB : options.rootA;
        if (!withinRoot(op.source.path, root) || !withinRoot(op.target.path, options.rootA) || samePath(op.source.path, op.target.path) ||
            op.source.name != op.target.name || op.source.name != QFileInfo(op.source.path).fileName() || op.target.name != QFileInfo(op.target.path).fileName() ||
            op.source.size != op.target.size || op.source.sha256 != op.target.sha256 || op.source.sha256.size() != 32 || op.source.streams != op.target.streams ||
            (op.source.fileId == op.target.fileId && op.source.volumeId == op.target.volumeId)) return QStringLiteral("替换清单无效或超出选定目录。");
        const QString source = cleanPath(op.source.path).toCaseFolded();
        if (sources.contains(source)) return QStringLiteral("清单包含重复的源文件。");
        sources.insert(source); targets.insert(cleanPath(op.target.path).toCaseFolded());
    }
    for (const auto &s : sources) if (targets.contains(s)) return QStringLiteral("清单将保留目标同时标记为替换。");
    return {};
}
}
ExecutionResult Executor::execute(const QVector<Operation> &plan, ScanOptions options, bool permanent, const QString &logs, const Cancel &cancel, const Progress &progress) {
    ExecutionResult result;
    QString error = validatePlan(plan, options);
    if (!error.isEmpty()) { result.failed = plan.size(); result.messages.append(error); return result; }
    win::ComScope com;
    if (!com.valid() || !QDir().mkpath(logs)) { result.failed = plan.size(); result.messages.append(QStringLiteral("无法初始化 Shell 或创建日志目录。")); return result; }
    result.logPath = QDir(logs).filePath("operation-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + "-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".jsonl");
    for (int i = 0; i < plan.size(); ++i) {
        if (cancel && cancel->load()) { result.cancelled = true; break; }
        if (progress) progress(QStringLiteral("重新校验并替换：") + plan[i].source.name, quint64(i), quint64(plan.size()));
        error.clear();
        if (replaceOne(plan[i], permanent, result.logPath, cancel, error)) {
            ++result.done; result.messages.append(QStringLiteral("成功：") + plan[i].source.path + " → " + plan[i].target.path);
            if (!error.isEmpty()) result.messages.append(error);
        } else {
            if (cancel && cancel->load()) { result.cancelled = true; break; }
            ++result.failed; result.messages.append(QStringLiteral("跳过/失败：") + plan[i].source.path + "：" + error);
        }
    }
    if (progress) progress(QStringLiteral("执行完成"), quint64(result.done + result.failed), quint64(plan.size()));
    return result;
}
ExecutionResult Executor::recover(const QString &journal, const Cancel &cancel, const Progress &progress) {
    ExecutionResult result; result.logPath = journal;
    QFile file(journal);
    if (!file.open(QIODevice::ReadOnly)) { result.failed = 1; result.messages.append(file.errorString()); return result; }
    QMap<QString, QJsonObject> entries;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine(1024 * 1024);
        QJsonParseError error; const auto doc = QJsonDocument::fromJson(line, &error);
        if (error.error == QJsonParseError::NoError && doc.isObject() && !doc.object()["id"].toString().isEmpty()) entries[doc.object()["id"].toString()] = doc.object();
    }
    file.close();
    win::ComScope com;
    if (!com.valid()) { result.failed = 1; result.messages.append(QStringLiteral("无法初始化 Shell。")); return result; }
    int i = 0;
    for (auto entry : entries) {
        if (cancel && cancel->load()) { result.cancelled = true; break; }
        if (progress) progress(QStringLiteral("恢复未完成的暂存事务"), ++i, entries.size());
        const bool committed = entry["phase"].toString() == "committed";
        if ((committed && entry["permanent"].toBool()) || entry["phase"].toString() == "rolled_back" || entry["phase"].toString() == "recovered") continue;
        const FileRecord source = fromJson(entry["source"].toObject()), target = fromJson(entry["target"].toObject());
        const QString staged = entry["staged"].toString(), link = entry["link"].toString(), temp = entry["tempLink"].toString();
        QString error;
        const QString id = entry["id"].toString();
        if (staged != QFileInfo(source.path).absolutePath() + "/.dupkiller-" + id + ".pending" || link != source.path + ".lnk" ||
            temp != QFileInfo(source.path).absolutePath() + "/.dupkiller-" + id + ".lnk" || !withinRoot(source.path, source.root) ||
            source.sha256.size() != 32 || source.name != QFileInfo(source.path).fileName() || source.name != target.name) {
            ++result.failed; result.messages.append(QStringLiteral("日志条目无效，未操作：") + id); continue;
        }
        std::vector<win::Handle> parents;
        if (!win::lockParents(source.path, parents, error)) { ++result.failed; result.messages.append(error); continue; }
        QString restorePath = staged;
        if (committed) {
            restorePath = cleanPath(entry["recycledPath"].toString());
            if (!restorePath.contains("/$Recycle.Bin/", Qt::CaseInsensitive) || !QFileInfo(restorePath).fileName().startsWith("$R")) {
                ++result.failed; result.messages.append(QStringLiteral("回收站路径无效，未操作：") + source.path); continue;
            }
            if (!win::lockParents(restorePath, parents, error)) { ++result.failed; result.messages.append(error); continue; }
        }
        const bool hasStaged = GetFileAttributesW(win::nativePath(restorePath).c_str()) != INVALID_FILE_ATTRIBUTES;
        const bool hasSource = GetFileAttributesW(win::nativePath(source.path).c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!hasStaged && !hasSource) {
            result.messages.append(QStringLiteral("暂存副本已移除；保留快捷方式，请结合回收站/日志检查：") + source.path); continue;
        }
        win::Handle original = openFile(hasStaged ? restorePath : source.path, true, committed ? FILE_SHARE_READ | FILE_SHARE_DELETE : FILE_SHARE_READ);
        if (!original.valid() || !verify(original.value, source, hasStaged ? restorePath : source.path, cancel, error) ||
            (hasStaged && (hasSource || (committed ? !win::restoreRecycled(QByteArray::fromBase64(entry["recycledItemId"].toString().toLatin1()), restorePath, source.path, error)
                                               : !win::renameHandle(original.value, source.path, error))))) {
            ++result.failed; result.messages.append(QStringLiteral("未覆盖任何文件，无法恢复：") + source.path + "；" + error); continue;
        }
        if (committed && entry.contains("recycleMetadata")) {
            // Some Shell items restore the data file but leave the associated $I
            // descriptor. Remove only the descriptor whose identity and full hash
            // were recorded immediately after this operation created it.
            const FileRecord descriptor = fromJson(entry["recycleMetadata"].toObject());
            const QString expectedPath = cleanPath(QFileInfo(restorePath).absolutePath() + "/$I" + QFileInfo(restorePath).fileName().mid(2));
            if (samePath(descriptor.path, expectedPath) && GetFileAttributesW(win::nativePath(descriptor.path).c_str()) != INVALID_FILE_ATTRIBUTES) {
                win::Handle meta = openFile(descriptor.path, true); FileRecord current; QByteArray hash; QString detail;
                FILE_DISPOSITION_INFO disposition{}; disposition.DeleteFile = TRUE;
                if (!meta.valid() || !win::metadata(meta.value, current, detail, true) || !win::unchanged(descriptor, current) ||
                    !win::hashHandle(meta.value, hash, {}, detail) || hash != descriptor.sha256 || descriptor.sha256.size() != 32 ||
                    !SetFileInformationByHandle(meta.value, FileDispositionInfo, &disposition, sizeof(disposition)))
                    result.messages.append(QStringLiteral("原文件已恢复；回收站索引已变化或无法清理，未移除：") + descriptor.path);
            }
        }
        if (GetFileAttributesW(win::nativePath(link).c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (!entry.contains("linkIdentity") || !samePath(win::shortcutTarget(link, error), target.path) ||
                !removeOwnedLink(link, fromJson(entry["linkIdentity"].toObject()), error)) {
                ++result.failed; result.messages.append(QStringLiteral("原文件已恢复；快捷方式需人工检查：") + link + "；" + error); continue;
            }
        }
        // Only remove our unique temporary shortcut after verifying its target.
        if (GetFileAttributesW(win::nativePath(temp).c_str()) != INVALID_FILE_ATTRIBUTES && samePath(win::shortcutTarget(temp, error), target.path)) DeleteFileW(win::nativePath(temp).c_str());
        if (!record(journal, entry, "recovered", error)) { ++result.failed; result.messages.append(error); continue; }
        ++result.done; result.messages.append(QStringLiteral("已恢复原文件：") + source.path);
    }
    return result;
}
}
