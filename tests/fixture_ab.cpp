#include "scanner.h"
#include "executor.h"
#include "filetreemodel.h"
#include "windowsfs.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

using namespace dup;
namespace {
QJsonObject fingerprint(const QString &path, QString &error) {
    win::Handle h(CreateFileW(win::nativePath(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    QByteArray hash; FileRecord f;
    if (!h.valid() || !win::metadata(h.value, f, error) || !win::hashHandle(h.value, hash, {}, error)) return {};
    QMap<QString, QByteArray> named; QJsonObject streams;
    if (!win::hashStreams(path, named, {}, error)) return {};
    for (auto it = named.constBegin(); it != named.constEnd(); ++it) streams[it.key()] = QString::fromLatin1(it.value().toHex());
    return {{"sha256", QString::fromLatin1(hash.toHex())}, {"bytes", QString::number(f.size)},
            {"writeTicks", QString::number(f.writeTicks)}, {"attributes", int(f.attributes)}, {"streams", streams}};
}
bool save(const QString &path, const QJsonObject &object) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(object).toJson()) > 0;
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOption({"source", "Read-only fixture source.", "directory"});
    parser.addOption({"output", "New isolated output directory. Must not exist.", "directory"});
    parser.addOption({"recycle", "Recycle duplicate copies instead of permanently deleting them."});
    parser.addOption({"native", "Use native enumeration instead of automatic NTFS acceleration."});
    parser.process(app);
    QTextStream console(stdout); QString error;
    const QString source = cleanPath(parser.value("source")), output = cleanPath(parser.value("output"));
    if (parser.value("source").isEmpty() || parser.value("output").isEmpty() || !QFileInfo(source).isDir() ||
        QFileInfo::exists(output) || withinRoot(output, source) || withinRoot(source, output)) return 2;
    const QString a = output + "/A", b = output + "/B";
    if (!QDir().mkpath(a) || !QDir().mkpath(b)) return 2;
    QJsonObject baseline;
    QDirIterator files(source, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    int copied = 0;
    while (files.hasNext()) {
        const QString path = files.next(), relative = QDir(source).relativeFilePath(path);
        const auto state = fingerprint(path, error); if (state.isEmpty()) return 3;
        baseline[relative] = state;
        for (const QString &root : {a, b}) {
            const QString dest = root + "/" + relative; QDir().mkpath(QFileInfo(dest).absolutePath());
            // CopyFileW preserves named NTFS streams, timestamps and file attributes.
            if (!CopyFileW(win::nativePath(path).c_str(), win::nativePath(dest).c_str(), TRUE)) return 4;
        }
        console << "Copied " << ++copied << '\n'; console.flush();
    }
    save(output + "/baseline.json", baseline);
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    ScanOptions options; options.mode = Mode::Dual; options.rootA = a; options.rootB = b;
    options.backend = parser.isSet("native") ? Backend::Native : Backend::Auto;
    auto progress = [&console](const QString &phase, quint64 done, quint64 total) {
        console << phase << " " << done << "/" << total << '\n'; console.flush();
    };
    const auto scan = Scanner::scan(options, cancel, progress);
    FileTreeModel model; model.setResult(scan);
    const auto plan = model.plan();
    const auto execution = Executor::execute(plan, options, !parser.isSet("recycle"), output + "/logs", cancel, progress);
    win::ComScope com; int links = 0, remaining = 0, sourceChanged = 0, aChanged = 0, streamsLost = 0, wrongTargets = 0;
    for (auto it = baseline.constBegin(); it != baseline.constEnd(); ++it) {
        const auto expected = it.value().toObject();
        sourceChanged += fingerprint(source + "/" + it.key(), error) != expected;
        aChanged += fingerprint(a + "/" + it.key(), error) != expected;
        const QString original = b + "/" + it.key(), link = original + ".lnk";
        remaining += QFileInfo::exists(original);
        if (QFileInfo::exists(link)) {
            ++links;
            const QString target = win::shortcutTarget(link, error);
            wrongTargets += !withinRoot(target, a) || !QFileInfo::exists(target);
            streamsLost += fingerprint(link, error)["streams"] != expected["streams"];
        }
    }
    QJsonArray scanMessages, executionMessages;
    for (const auto &line : scan.messages) scanMessages.append(line);
    for (const auto &line : execution.messages) executionMessages.append(line);
    QJsonObject report{{"qt", QString::fromLatin1(qVersion())}, {"source", source}, {"output", output},
        {"copiedFiles", copied}, {"planned", plan.size()}, {"done", execution.done}, {"failed", execution.failed},
        {"shortcuts", links}, {"remainingOriginalsInB", remaining}, {"sourceChanged", sourceChanged}, {"aChanged", aChanged},
        {"streamsLost", streamsLost}, {"wrongTargets", wrongTargets}, {"scanSkipped", QString::number(scan.skipped)},
        {"scanError", scan.error}, {"journal", execution.logPath}, {"permanent", !parser.isSet("recycle")},
        {"scanMessages", scanMessages}, {"executionMessages", executionMessages}};
    save(output + "/report.json", report);
    report.remove("scanMessages"); report.remove("executionMessages");
    console << QJsonDocument(report).toJson(QJsonDocument::Compact) << '\n';
    return links == copied && !remaining && !sourceChanged && !aChanged && !streamsLost && !wrongTargets && !execution.failed ? 0 : 1;
}
