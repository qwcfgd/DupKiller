#include "scanner.h"
#include "executor.h"
#include "windowsfs.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <QDir>
#include <mutex>

using namespace dup;
namespace {
QJsonObject fileJson(const FileRecord &f) {
    QJsonObject streams; for (auto it = f.streams.constBegin(); it != f.streams.constEnd(); ++it) streams[it.key()] = QString::fromLatin1(it.value().toHex());
    return {{"path", f.path}, {"root", f.root}, {"name", f.name}, {"side", f.side}, {"size", QString::number(f.size)},
        {"fileId", QString::number(f.fileId)}, {"volume", QString::number(f.volumeId)}, {"writeTicks", QString::number(f.writeTicks)},
        {"attributes", QString::number(f.attributes)}, {"sha256", QString::fromLatin1(f.sha256.toHex())}, {"streams", streams}};
}
FileRecord readFile(const QJsonObject &o) {
    FileRecord f; f.path = o["path"].toString(); f.root = o["root"].toString(); f.name = o["name"].toString(); f.side = o["side"].toString();
    f.size = o["size"].toString().toULongLong(); f.fileId = o["fileId"].toString().toULongLong(); f.volumeId = o["volume"].toString().toULongLong();
    f.writeTicks = o["writeTicks"].toString().toULongLong(); f.attributes = o["attributes"].toString().toUInt();
    f.sha256 = QByteArray::fromHex(o["sha256"].toString().toLatin1()); const auto streams = o["streams"].toObject();
    for (auto it = streams.constBegin(); it != streams.constEnd(); ++it) f.streams[it.key()] = QByteArray::fromHex(it.value().toString().toLatin1());
    return f;
}
bool save(const QString &path, const QJsonObject &o) {
    QSaveFile file(path); const auto bytes = QJsonDocument(o).toJson();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
QJsonArray messages(const QStringList &lines) { QJsonArray out; for (const auto &line : lines) out.append(line); return out; }
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); QCoreApplication::setApplicationName("DupKillerTask");
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOption({"root", "Single-directory read-only scan root.", "directory"});
    parser.addOption({"photos", "Restrict scanning to image and camera RAW extensions."});
    parser.addOption({"prefer-non-dated", "Prefer originals outside date-plus-description directories."});
    parser.addOption({"hash-workers", "Override automatic disk readers (1..4).", "count"});
    parser.addOption({"no-sampling", "Disable the safe prefix rejection filter for comparison benchmarks."});
    parser.addOption({"save-plan", "Save a reviewed scan plan; scanning never changes source files.", "json"});
    parser.addOption({"execute-plan", "Execute a saved plan with full revalidation; copies go to the recycle bin.", "json"});
    parser.addOption({"report", "Write detailed private scan/execution report.", "json"});
    parser.process(app); QTextStream console(stdout);
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    QElapsedTimer elapsed, progressClock; elapsed.start(); progressClock.start(); std::mutex outputMutex; QString lastPhase;
    const Progress progress = [&](const QString &phase, quint64 done, quint64 total) {
        std::lock_guard<std::mutex> lock(outputMutex);
        if (phase == lastPhase && progressClock.elapsed() < 1000 && (!total || done < total)) return;
        progressClock.restart(); lastPhase = phase;
        console << phase << " " << done << "/" << total << '\n'; console.flush();
    };
    ScanOptions options; QVector<Operation> plan; QJsonObject report;
    if (parser.isSet("execute-plan")) {
        if (parser.isSet("root") || parser.isSet("save-plan")) return 2;
        QFile file(parser.value("execute-plan")); if (!file.open(QIODevice::ReadOnly)) return 2;
        QJsonParseError error; report = QJsonDocument::fromJson(file.readAll(), &error).object();
        if (error.error != QJsonParseError::NoError || report["version"].toInt() != 1 || report["status"].toString() != "scanned") return 2;
        options.rootA = report["root"].toString(); options.photosOnly = report["photosOnly"].toBool();
        for (const auto &entry : report["operations"].toArray()) {
            const auto o = entry.toObject(); Operation op{readFile(o["source"].toObject()), readFile(o["target"].toObject())};
            if (options.photosOnly && (!photoName(op.source.name) || !photoName(op.target.name))) return 2;
            plan.append(op);
        }
        QString rootError = validateRoots(options); if (!rootError.isEmpty()) { console << rootError << '\n'; return 2; }
        const QString reportPath = parser.value("report"); if (reportPath.isEmpty() || samePath(reportPath, parser.value("execute-plan"))) return 2;
        const auto logs = QFileInfo(reportPath).absolutePath() + "/logs";
        report["status"] = "executing"; report["permanent"] = false;
        if (!save(reportPath, report)) return 2;
        console << "Execute " << plan.size() << " verified duplicates to recycle bin.\n"; console.flush();
        const auto result = Executor::execute(plan, options, false, logs, cancel, progress);
        report["done"] = result.done; report["failed"] = result.failed; report["journal"] = result.logPath;
        report["recycleQueries"] = result.recycleQueries;
        report["executionMessages"] = messages(result.messages); report["executionMs"] = double(elapsed.elapsed());
        win::ComScope com; int links = 0, remaining = 0, changedTargets = 0; QSet<QString> checkedTargets;
        QJsonArray verificationErrors;
        for (const auto &op : plan) {
            QString error; const QString link = op.source.path + ".lnk";
            const bool exists = GetFileAttributesW(win::nativePath(op.source.path).c_str()) != INVALID_FILE_ATTRIBUTES;
            remaining += exists;
            if (!exists && samePath(win::shortcutTarget(link, error), op.target.path)) ++links;
            else verificationErrors.append(op.source.path + QStringLiteral("：原文件仍存在或快捷方式目标不符。") + error);
            if (!checkedTargets.contains(op.target.path)) {
                FileRecord current;
                if (!win::inspect(op.target.path, op.target.root, op.target.side, current, error) || !win::unchanged(op.target, current)) { ++changedTargets; verificationErrors.append(op.target.path + QStringLiteral("：保留原件元数据变化。") + error); }
                checkedTargets.insert(op.target.path);
            }
        }
        report["verifiedLinks"] = links; report["remainingCopies"] = remaining; report["changedTargets"] = changedTargets; report["verificationErrors"] = verificationErrors;
        report["status"] = "executed";
        if (!save(reportPath, report)) return 2;
        console << "Completed " << result.done << ", failed " << result.failed << ", verified links " << links << ", remaining copies " << remaining << ", changed targets " << changedTargets << '\n'; console.flush();
        return result.failed || result.cancelled || links != plan.size() || changedTargets ? 1 : 0;
    }
    if (!parser.isSet("root")) return 2;
    options.rootA = parser.value("root"); options.photosOnly = parser.isSet("photos"); options.sampling = !parser.isSet("no-sampling");
    if (parser.isSet("hash-workers")) { options.hashWorkers = parser.value("hash-workers").toInt(); if (options.hashWorkers < 1 || options.hashWorkers > 4) return 2; }
    auto scan = Scanner::scan(options, cancel, progress);
    if (!scan.error.isEmpty() || scan.cancelled) { console << scan.error << '\n'; return 2; }
    QJsonArray operations; quint64 bytes = 0; int datedSources = 0, datedTargets = 0;
    for (auto &group : scan.groups) {
        if (parser.isSet("prefer-non-dated")) setKeeper(group, Mode::Single, pickKeeper(group, Mode::Single, KeepRule::PreferNonDated));
        datedTargets += datedDescriptionFolder(group.files[group.keeper]);
        for (int f = 0; f < group.files.size(); ++f) if (group.replace[f]) {
            const Operation op{group.files[f], group.files[group.keeper]}; bytes += op.source.size; datedSources += datedDescriptionFolder(op.source);
            operations.append(QJsonObject{{"source", fileJson(op.source)}, {"target", fileJson(op.target)}});
        }
    }
    report = {{"version", 1}, {"status", "scanned"}, {"root", scan.options.rootA}, {"photosOnly", options.photosOnly}, {"preferNonDated", parser.isSet("prefer-non-dated")},
        {"permanent", false}, {"operations", operations}, {"groups", scan.groups.size()}, {"planned", operations.size()}, {"duplicateBytes", QString::number(bytes)},
        {"datedSources", datedSources}, {"otherSources", operations.size() - datedSources}, {"datedTargetGroups", datedTargets},
        {"enumerated", QString::number(scan.enumerated)}, {"inspected", QString::number(scan.inspected)}, {"sampled", QString::number(scan.sampled)},
        {"sampleRejected", QString::number(scan.sampleRejected)}, {"hashed", QString::number(scan.hashed)}, {"hashBytes", QString::number(scan.hashBytes)},
        {"hashWorkers", scan.hashWorkers}, {"skipped", QString::number(scan.skipped)}, {"enumerateMs", double(scan.enumerateMs)}, {"sampleMs", double(scan.sampleMs)},
        {"hashMs", double(scan.hashMs)}, {"scanMs", double(elapsed.elapsed())}, {"messages", messages(scan.messages)}};
    if (parser.isSet("save-plan") && !save(parser.value("save-plan"), report)) return 2;
    if (parser.isSet("report") && !save(parser.value("report"), report)) return 2;
    console << "Scanned " << scan.enumerated << ", inspected " << scan.inspected << ", full SHA-256 " << scan.hashed << ", groups " << scan.groups.size()
        << ", duplicate copies " << operations.size() << ", bytes " << bytes << ", date-directory copies " << datedSources << ", milliseconds " << elapsed.elapsed() << '\n'; console.flush();
    return 0;
}
