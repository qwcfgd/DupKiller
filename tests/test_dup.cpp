#include "scanner.h"
#include "executor.h"
#include "windowsfs.h"
#include "filetreemodel.h"
#include "mainwindow.h"
#include "comparisonview.h"
#include <QtTest>
#include <QAbstractItemModelTester>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTreeView>
#include <winioctl.h>
#include <cstring>

using namespace dup;
namespace {
Cancel token(bool value = false) { return std::make_shared<std::atomic_bool>(value); }
QString createFile(const QString &root, const QString &relative, const QByteArray &bytes) {
    const QString path = QDir(root).filePath(relative);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path); if (!file.open(QIODevice::WriteOnly)) return {};
    if (file.write(bytes) != bytes.size()) return {};
    file.close(); return cleanPath(path);
}
QByteArray content(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
ScanResult scan(const QString &a, const QString &b = {}, Backend backend = Backend::Native) {
    ScanOptions options; options.rootA = a; options.rootB = b; options.backend = backend; options.mode = b.isEmpty() ? Mode::Single : Mode::Dual;
    return Scanner::scan(options, token());
}
QModelIndex fileIndex(FileTreeModel &model, const QString &path, const QModelIndex &parent = {}) {
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const auto i = model.index(row, 0, parent);
        if (!i.data(FileTreeModel::FolderRole).toBool() && samePath(i.data(FileTreeModel::PathRole).toString(), path)) return i;
        const auto child = fileIndex(model, path, i); if (child.isValid()) return child;
    }
    return {};
}
QModelIndex pairIndex(ComparisonModel &model, const QString &path, const QModelIndex &parent = {}) {
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const auto i = model.index(row, 0, parent);
        if (!i.data(FileTreeModel::FolderRole).toBool() && samePath(i.sibling(row, ComparisonModel::CandidateName).data(FileTreeModel::PathRole).toString(), path)) return i;
        const auto child = pairIndex(model, path, i); if (child.isValid()) return child;
    }
    return {};
}
QJsonObject serialized(const FileRecord &f) {
    return {{"path", f.path}, {"root", f.root}, {"name", f.name}, {"side", f.side}, {"size", QString::number(f.size)},
        {"fileId", QString::number(f.fileId)}, {"volume", QString::number(f.volumeId)}, {"writeTicks", QString::number(f.writeTicks)},
        {"attributes", QString::number(f.attributes)}, {"sha256", QString::fromLatin1(f.sha256.toHex())}};
}
}
class DupTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QCoreApplication::setOrganizationName("QtDupKillerTests"); QCoreApplication::setApplicationName("QtDupKillerTests"); QDir().mkpath("test-output"); }
    void singleComparison() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX");
        const QString keep = createFile(dir.path(), "file.bin", "abcd");
        createFile(dir.path(), "x/file.bin", "abcd"); createFile(dir.path(), "deep/y/file.bin", "abcd");
        createFile(dir.path(), "other.bin", "abcd"); createFile(dir.path(), "z/file.bin", "wxyz");
        createFile(dir.path(), "different/file.bin", "abcde"); createFile(dir.path(), "case/FILE.bin", "abcd");
        const auto result = scan(dir.path());
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error)); QCOMPARE(result.groups.size(), 1);
        const auto &g = result.groups[0]; QCOMPARE(g.files.size(), 3); QCOMPARE(g.files[g.keeper].path, keep);
        QCOMPARE(result.hashed, quint64(4)); QCOMPARE(content(keep), QByteArray("abcd"));
        for (const auto &f : g.files) QCOMPARE(f.sha256, QCryptographicHash::hash("abcd", QCryptographicHash::Sha256));
    }
    void dualProtectsA() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX"); const QString a = dir.path() + "/A", b = dir.path() + "/B";
        const QString target = createFile(a, "item.txt", "data"); createFile(a, "copy/item.txt", "data");
        createFile(b, "elsewhere/item.txt", "data"); createFile(b, "copy/item.txt", "data");
        createFile(b, "only/b.txt", "Bonly"); createFile(b, "another/b.txt", "Bonly");
        const auto result = scan(a, b); QCOMPARE(result.groups.size(), 1);
        FileTreeModel model; model.setResult(result); QCOMPARE(model.plan().size(), 2);
        for (const auto &op : model.plan()) { QVERIFY(withinRoot(op.source.path, b)); QCOMPARE(op.target.path, target); }
        const auto bFile = fileIndex(model, model.plan()[0].source.path);
        QVERIFY(!model.chooseSelected({bFile}).isEmpty()); QCOMPARE(model.plan().size(), 2);
    }
    void excludedAndHiddenFiles() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX");
        const auto first = createFile(dir.path(), "h.txt", "hidden"); const auto second = createFile(dir.path(), "x/h.txt", "hidden");
        QVERIFY(SetFileAttributesW(win::nativePath(first).c_str(), FILE_ATTRIBUTE_HIDDEN));
        const auto hard = createFile(dir.path(), "hard/h.txt", "hidden");
        QVERIFY(CreateHardLinkW(win::nativePath(dir.path() + "/hard2/h.txt").c_str(), win::nativePath(hard).c_str(), nullptr) == FALSE);
        QDir().mkpath(dir.path() + "/hard2"); QVERIFY(CreateHardLinkW(win::nativePath(dir.path() + "/hard2/h.txt").c_str(), win::nativePath(hard).c_str(), nullptr));
        const auto system = createFile(dir.path(), "sys/h.txt", "hidden"); QVERIFY(SetFileAttributesW(win::nativePath(system).c_str(), FILE_ATTRIBUTE_SYSTEM));
        const auto ads = createFile(dir.path(), "ads/h.txt", "hidden"); QVERIFY(!createFile(dir.path(), "ads/h.txt:extra", "additional").isEmpty());
        createFile(dir.path(), "existing.lnk", "not a link");
        const auto result = scan(dir.path()); QCOMPARE(result.groups.size(), 1); QCOMPARE(result.groups[0].files.size(), 2);
        QVERIFY(result.skipped >= 4); QVERIFY(win::hasExtraStreams(ads)); QCOMPARE(content(second), QByteArray("hidden"));
        SetFileAttributesW(win::nativePath(first).c_str(), FILE_ATTRIBUTE_NORMAL); SetFileAttributesW(win::nativePath(system).c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    void overlappingRoots() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX"); QDir().mkpath(dir.path() + "/nested");
        QVERIFY(!scan(dir.path(), dir.path() + "/nested").error.isEmpty()); QVERIFY(!scan(dir.path(), dir.path()).error.isEmpty());
        ScanOptions options; QVERIFY(!validateRoots(options).isEmpty());
    }
    void namedStreamsMatchAndPreserve_data() {
        QTest::addColumn<bool>("permanent");
        QTest::newRow("permanent") << true;
        QTest::newRow("recycle-and-recover") << false;
    }
    void namedStreamsMatchAndPreserve() {
        QFETCH(bool, permanent);
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX");
        const auto a = dir.path() + "/A", b = dir.path() + "/B";
        const QString original = createFile(a, "download.exe", "test only, never executed");
        const QString duplicate = createFile(b, "nested/download.exe", "test only, never executed");
        for (const auto &path : {original, duplicate}) {
            QVERIFY(!createFile(QFileInfo(path).absolutePath(), QFileInfo(path).fileName() + ":Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\n").isEmpty());
            QVERIFY(!createFile(QFileInfo(path).absolutePath(), QFileInfo(path).fileName() + ":SmartScreen", "testing").isEmpty());
            QVERIFY(!createFile(QFileInfo(path).absolutePath(), QFileInfo(path).fileName() + ":empty", {}).isEmpty());
        }
        FileTreeModel model; model.setResult(scan(a, b)); QCOMPARE(model.plan().size(), 1);
        const auto op = model.plan()[0]; QCOMPARE(op.source.streams.size(), 3);
        const auto result = Executor::execute(model.plan(), model.result().options, permanent, dir.path() + "/logs", token());
        QVERIFY2(result.done == 1 && result.failed == 0, qPrintable(result.messages.join('\n')));
        QVERIFY(!QFileInfo::exists(duplicate)); QVERIFY(QFileInfo::exists(duplicate + ".lnk"));
        QString error; QMap<QString, QByteArray> streams;
        QVERIFY2(win::hashStreams(duplicate + ".lnk", streams, {}, error), qPrintable(error)); QCOMPARE(streams, op.source.streams);
        win::ComScope com; QCOMPARE(win::shortcutTarget(duplicate + ".lnk", error), original);
        QVERIFY(win::hashStreams(original, streams, {}, error)); QCOMPARE(streams, op.target.streams);
        if (!permanent) {
            const auto restored = Executor::recover(result.logPath, token());
            QVERIFY2(restored.done == 1 && restored.failed == 0, qPrintable(restored.messages.join('\n')));
            QVERIFY(win::hashStreams(duplicate, streams, {}, error)); QCOMPARE(streams, op.source.streams);
            QVERIFY(!QFileInfo::exists(duplicate + ".lnk"));
        }
    }
    void differingNamedStreamsRemainSeparate() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX");
        const auto a = dir.path() + "/A", b = dir.path() + "/B";
        createFile(a, "item.txt", "same"); createFile(b, "item.txt", "same");
        createFile(a, "item.txt:extra", "first"); createFile(b, "item.txt:extra", "other");
        const auto result = scan(a, b); QCOMPARE(result.hashed, quint64(2)); QVERIFY(result.groups.isEmpty());
        QCOMPARE(content(b + "/item.txt:extra"), QByteArray("other"));
    }
    void changedNamedStreamRefusesReplacement() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX");
        const auto a = dir.path() + "/A", b = dir.path() + "/B";
        createFile(a, "item.txt", "same"); const auto duplicate = createFile(b, "item.txt", "same");
        createFile(a, "item.txt:extra", "first"); createFile(b, "item.txt:extra", "first");
        FileTreeModel model; model.setResult(scan(a, b)); QCOMPARE(model.plan().size(), 1);
        createFile(b, "item.txt:extra", "other");
        // Restore the timestamp to exercise stream hashing rather than metadata rejection.
        win::Handle h(CreateFileW(win::nativePath(duplicate).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
        const quint64 ticks = model.plan()[0].source.writeTicks; FILETIME time{DWORD(ticks), DWORD(ticks >> 32)};
        QVERIFY(h.valid()); QVERIFY(SetFileTime(h.value, nullptr, nullptr, &time)); h.reset();
        const auto result = Executor::execute(model.plan(), model.result().options, true, dir.path() + "/logs", token());
        QCOMPARE(result.done, 0); QCOMPARE(result.failed, 1); QVERIFY(QFileInfo::exists(duplicate)); QVERIFY(!QFileInfo::exists(duplicate + ".lnk"));
        QVERIFY(result.messages.join('\n').contains(QStringLiteral("数据流")));
    }
    void cancellation() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX"); createFile(dir.path(), "a.bin", "data"); createFile(dir.path(), "x/a.bin", "data");
        ScanOptions options; options.rootA = dir.path(); options.backend = Backend::Native;
        const auto result = Scanner::scan(options, token(true)); QVERIFY(result.cancelled); QVERIFY(result.groups.isEmpty());
    }
    void lockedFile() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX"); const auto locked = createFile(dir.path(), "a.bin", "data"); createFile(dir.path(), "x/a.bin", "data");
        win::Handle h(CreateFileW(win::nativePath(locked).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr)); QVERIFY(h.valid());
        const auto result = scan(dir.path()); QVERIFY(result.groups.isEmpty()); QVERIFY(result.skipped > 0);
    }
    void automaticBackendMatchesNative() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX"); createFile(dir.path(), "one.txt", "same"); createFile(dir.path(), "nested/one.txt", "same");
        const auto native = scan(dir.path()), automatic = scan(dir.path(), {}, Backend::Auto);
        QVERIFY2(native.error.isEmpty(), qPrintable(native.error)); QVERIFY2(automatic.error.isEmpty(), qPrintable(automatic.error));
        QCOMPARE(automatic.groups.size(), native.groups.size()); QCOMPARE(automatic.groups.size(), 1); QCOMPARE(automatic.groups[0].files.size(), native.groups[0].files.size());
        QCOMPARE(automatic.groups[0].files[automatic.groups[0].keeper].path, native.groups[0].files[native.groups[0].keeper].path);
        QVERIFY(!automatic.messages.isEmpty());
    }
    void usnParser() {
        const QString name = "test.txt"; const int header = int(offsetof(USN_RECORD, FileName));
        QByteArray buffer(8 + header + name.size() * 2, '\0'); USN_RECORD record{};
        record.RecordLength = DWORD(buffer.size() - 8); record.MajorVersion = 2; record.FileNameOffset = WORD(header);
        record.FileNameLength = WORD(name.size() * 2); record.FileReferenceNumber = 7; record.ParentFileReferenceNumber = 5; record.Usn = 12;
        std::memcpy(buffer.data() + 8, &record, size_t(header)); std::memcpy(buffer.data() + 8 + header, name.utf16(), size_t(name.size() * 2));
        QHash<quint64, MftEntry> entries; QString error;
        QVERIFY(parseUsnBuffer(buffer, entries, false, 0, error)); QCOMPARE(entries[7].name, name);
        record.Reason = USN_REASON_FILE_DELETE; std::memcpy(buffer.data() + 8, &record, size_t(header));
        QVERIFY(parseUsnBuffer(buffer, entries, true, 13, error)); QVERIFY(entries.isEmpty());
        record.MajorVersion = 3; std::memcpy(buffer.data() + 8, &record, size_t(header)); QVERIFY(!parseUsnBuffer(buffer, entries, false, 0, error));
        record.MajorVersion = 2; record.FileNameLength = 65534; std::memcpy(buffer.data() + 8, &record, size_t(header)); QVERIFY(!parseUsnBuffer(buffer, entries, false, 0, error));
        QVERIFY(!parseUsnBuffer(QByteArray(4, '\0'), entries, false, 0, error));
        record.FileNameLength = 2; record.FileAttributes = FILE_ATTRIBUTE_DIRECTORY; record.ParentFileReferenceNumber = record.FileReferenceNumber;
        std::memcpy(buffer.data() + 8, &record, size_t(header)); const wchar_t dot = L'.'; std::memcpy(buffer.data() + 8 + header, &dot, 2);
        QVERIFY(parseUsnBuffer(buffer, entries, false, 0, error)); QCOMPARE(entries[7].name, QString("."));
    }
    void modelSelectionAndTooltip() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX"); const auto shallow = createFile(dir.path(), "a.txt", "same"), deeper = createFile(dir.path(), "x/a.txt", "same");
        createFile(dir.path(), "b.jpg", "image"); createFile(dir.path(), "x/b.jpg", "image");
        FileTreeModel model; QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.setResult(scan(dir.path())); QCOMPARE(model.plan().size(), 2);
        const auto selected = fileIndex(model, deeper); QVERIFY(selected.isValid());
        QVERIFY(model.chooseSelected({selected}).isEmpty());
        const auto original = fileIndex(model, shallow); QCOMPARE(original.sibling(original.row(), FileTreeModel::Replace).data(Qt::CheckStateRole).toInt(), int(Qt::Checked));
        QVERIFY(selected.data(Qt::ToolTipRole).toString().contains("SHA-256"));
        for (int col = 0; col < model.columnCount(); ++col) QVERIFY(!selected.sibling(selected.row(), col).data().toString().contains(QString::fromLatin1(QCryptographicHash::hash("same", QCryptographicHash::Sha256).toHex())));
        QPersistentModelIndex persistent(selected); model.sort(FileTreeModel::Modified, Qt::DescendingOrder); QVERIFY(persistent.isValid()); QCOMPARE(persistent.data(FileTreeModel::PathRole).toString(), deeper);
        model.setReplacement({original}, false); QCOMPARE(model.plan().size(), 1);
        model.applyRule({}, KeepRule::Shallowest); QCOMPARE(model.plan().size(), 2);
        model.preferFolders({model.parent(fileIndex(model, deeper))});
        QCOMPARE(model.result().groups[0].files[model.result().groups[0].keeper].path, deeper);
    }
    void orderingAndBatchRules() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX");
        const auto a = createFile(dir.path(), "A.jpg", "image"), z = createFile(dir.path(), "Z.txt", "text");
        const auto aCopy = createFile(dir.path(), "copies/A.jpg", "image"), zCopy = createFile(dir.path(), "copies/Z.txt", "text");
        const quint64 early = 133000000000000000ULL, late = early + 1000000000ULL;
        for (const auto &p : {a, z, aCopy, zCopy}) {
            const auto time = p == a || p == z ? late : early;
            win::Handle h(CreateFileW(win::nativePath(p).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
            FILETIME ft{DWORD(time), DWORD(time >> 32)}; QVERIFY(SetFileTime(h.value, nullptr, nullptr, &ft));
        }
        FileTreeModel model; model.setResult(scan(dir.path())); QCOMPARE(model.result().groups.size(), 2);
        model.applyRule({}, KeepRule::Oldest);
        for (const auto &g : model.result().groups) QCOMPARE(g.files[g.keeper].depth, 1);
        model.applyRule({}, KeepRule::Newest);
        for (const auto &g : model.result().groups) QCOMPARE(g.files[g.keeper].depth, 0);
        const auto rootIndex = model.index(0, 0);
        model.sort(FileTreeModel::Name, Qt::DescendingOrder);
        QVERIFY(model.index(0, 0, rootIndex).data(FileTreeModel::FolderRole).toBool());
        QCOMPARE(model.index(1, 0, rootIndex).data().toString(), QString("Z.txt"));
        QCOMPARE(model.index(2, 0, rootIndex).data().toString(), QString("A.jpg"));
        model.sort(FileTreeModel::Type, Qt::AscendingOrder);
        QCOMPARE(model.index(1, 0, rootIndex).data().toString(), QString("A.jpg"));
        model.sort(FileTreeModel::Type, Qt::DescendingOrder);
        QCOMPARE(model.index(1, 0, rootIndex).data().toString(), QString("Z.txt"));
        QVERIFY(model.chooseSelected({fileIndex(model, aCopy), fileIndex(model, zCopy)}).isEmpty());
        for (const auto &g : model.result().groups) QCOMPARE(g.files[g.keeper].depth, 1);
    }
    void parentLeasePreventsAncestorRename() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX");
        const auto file = createFile(dir.path(), "ancestor/child/file.txt", "content");
        std::vector<win::Handle> locks; QString error; QVERIFY2(win::lockParents(file, locks, error), qPrintable(error));
        const auto oldParent = dir.path() + "/ancestor", newParent = dir.path() + "/renamed";
        QVERIFY(withinRoot(oldParent, dir.path()) && withinRoot(newParent, dir.path()));
        QVERIFY(!MoveFileExW(win::nativePath(oldParent).c_str(), win::nativePath(newParent).c_str(), 0));
        QCOMPARE(content(file), QByteArray("content"));
    }
    void permanentReplacement_data() { QTest::addColumn<QByteArray>("bytes"); QTest::newRow("normal") << QByteArray("original content"); QTest::newRow("empty") << QByteArray(); }
    void permanentReplacement() {
        QFETCH(QByteArray, bytes); QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX"); const auto original = createFile(dir.path(), "文件.txt", bytes), duplicate = createFile(dir.path(), "子目录/文件.txt", bytes);
        FileTreeModel model; model.setResult(scan(dir.path())); QCOMPARE(model.plan().size(), 1);
        const auto result = Executor::execute(model.plan(), model.result().options, true, dir.path() + "/logs", token());
        QVERIFY2(result.done == 1, qPrintable(result.messages.join('\n'))); QCOMPARE(result.failed, 0); QVERIFY2(!QFileInfo::exists(duplicate), qPrintable(result.messages.join('\n')));
        QVERIFY(QFileInfo::exists(duplicate + ".lnk")); QCOMPARE(content(original), bytes);
        win::ComScope com; QString error; QCOMPARE(win::shortcutTarget(duplicate + ".lnk", error), original);
        QVERIFY(QFileInfo::exists(result.logPath)); QVERIFY(content(result.logPath).contains("committed"));
        QVERIFY(scan(dir.path()).groups.isEmpty());
    }
    void changedAfterScan() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX"); createFile(dir.path(), "a.txt", "same"); const auto copy = createFile(dir.path(), "x/a.txt", "same");
        FileTreeModel model; model.setResult(scan(dir.path())); const auto expected = model.plan()[0].source;
        createFile(dir.path(), "x/a.txt", "DIFF");
        win::Handle h(CreateFileW(win::nativePath(copy).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
        FILETIME time{DWORD(expected.writeTicks), DWORD(expected.writeTicks >> 32)}; QVERIFY(SetFileTime(h.value, nullptr, nullptr, &time)); h.reset();
        const auto result = Executor::execute(model.plan(), model.result().options, true, dir.path() + "/logs", token());
        QCOMPARE(result.done, 0); QCOMPARE(result.failed, 1); QCOMPARE(content(copy), QByteArray("DIFF")); QVERIFY(!QFileInfo::exists(copy + ".lnk"));
    }
    void shortcutCollision() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX"); createFile(dir.path(), "a.txt", "same"); const auto copy = createFile(dir.path(), "x/a.txt", "same");
        FileTreeModel model; model.setResult(scan(dir.path())); createFile(dir.path(), "x/a.txt.lnk", "existing");
        const auto result = Executor::execute(model.plan(), model.result().options, true, dir.path() + "/logs", token());
        QCOMPARE(result.done, 0); QCOMPARE(content(copy), QByteArray("same")); QCOMPARE(content(copy + ".lnk"), QByteArray("existing"));
    }
    void executionCancelled() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX"); createFile(dir.path(), "a.txt", "same"); const auto copy = createFile(dir.path(), "x/a.txt", "same");
        FileTreeModel model; model.setResult(scan(dir.path()));
        const auto result = Executor::execute(model.plan(), model.result().options, true, dir.path() + "/logs", token(true));
        QVERIFY(result.cancelled); QCOMPARE(result.done, 0); QCOMPARE(content(copy), QByteArray("same"));
    }
    void dualExecution() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX"); const auto a = dir.path() + "/A", b = dir.path() + "/B";
        const auto first = createFile(a, "a.txt", "same"), aCopy = createFile(a, "x/a.txt", "same"), bCopy = createFile(b, "a.txt", "same");
        createFile(b, "x/b.txt", "onlyB"); createFile(b, "y/b.txt", "onlyB");
        FileTreeModel model; model.setResult(scan(a, b));
        const auto result = Executor::execute(model.plan(), model.result().options, true, dir.path() + "/logs", token());
        QVERIFY2(result.done == 1, qPrintable(result.messages.join('\n'))); QCOMPARE(content(first), QByteArray("same")); QCOMPARE(content(aCopy), QByteArray("same"));
        QVERIFY(!QFileInfo::exists(bCopy)); QVERIFY(QFileInfo::exists(b + "/x/b.txt")); QVERIFY(QFileInfo::exists(b + "/y/b.txt"));
    }
    void recycleReplacement() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX"); createFile(dir.path(), "a.txt", "same"); const auto copy = createFile(dir.path(), "x/a.txt", "same");
        FileTreeModel model; model.setResult(scan(dir.path()));
        const auto result = Executor::execute(model.plan(), model.result().options, false, dir.path() + "/logs", token());
        QVERIFY2(result.done == 1, qPrintable(result.messages.join('\n'))); QVERIFY(!QFileInfo::exists(copy)); QVERIFY(QFileInfo::exists(copy + ".lnk"));
        QByteArray logBytes = content(result.logPath); const auto lines = logBytes.split('\n'); QJsonObject committed;
        for (const auto &line : lines) { const auto o = QJsonDocument::fromJson(line).object(); if (o["phase"].toString() == "committed") committed = o; }
        const QString recycled = committed["recycledPath"].toString(); QVERIFY(QFileInfo::exists(recycled));
        const auto restored = Executor::recover(result.logPath, token());
        QVERIFY2(restored.done == 1, qPrintable(restored.messages.join('\n'))); QCOMPARE(content(copy), QByteArray("same")); QVERIFY(!QFileInfo::exists(copy + ".lnk"));
        QVERIFY(!QFileInfo::exists(recycled)); const QString metadata = QFileInfo(recycled).absolutePath() + "/$I" + QFileInfo(recycled).fileName().mid(2);
        QVERIFY(!QFileInfo::exists(metadata));
    }
    void recoverInterruptedTransaction() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX"); createFile(dir.path(), "a.txt", "same"); const auto copy = createFile(dir.path(), "x/a.txt", "same");
        FileTreeModel model; model.setResult(scan(dir.path())); const auto op = model.plan()[0];
        const QString id = "test-interruption", staged = QFileInfo(copy).absolutePath() + "/.dupkiller-" + id + ".pending";
        win::ComScope com; QString error; QVERIFY(win::createShortcut(copy + ".lnk", op.target.path, error));
        win::Handle link(CreateFileW(win::nativePath(copy + ".lnk").c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
        FileRecord identity; identity.path = copy + ".lnk"; QVERIFY(win::metadata(link.value, identity, error));
        link.reset(); link = win::Handle(CreateFileW(win::nativePath(identity.path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
        QVERIFY(win::hashHandle(link.value, identity.sha256, {}, error)); link.reset();
        QVERIFY(MoveFileExW(win::nativePath(copy).c_str(), win::nativePath(staged).c_str(), MOVEFILE_WRITE_THROUGH));
        QJsonObject entry{{"id", id}, {"phase", "staged"}, {"source", serialized(op.source)}, {"target", serialized(op.target)}, {"linkIdentity", serialized(identity)},
            {"staged", staged}, {"link", copy + ".lnk"}, {"tempLink", QFileInfo(copy).absolutePath() + "/.dupkiller-" + id + ".lnk"}, {"permanent", true}};
        const QString journal = createFile(dir.path(), "interrupted.jsonl", QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n');
        const auto result = Executor::recover(journal, token()); QVERIFY2(result.done == 1, qPrintable(result.messages.join('\n')));
        QCOMPARE(content(copy), QByteArray("same")); QVERIFY(!QFileInfo::exists(copy + ".lnk")); QVERIFY(!QFileInfo::exists(staged));
        QCOMPARE(Executor::recover(journal, token()).done, 0);
    }
    void interfaceAndAsyncScan() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX"); createFile(dir.path(), "a.txt", "same"); createFile(dir.path(), "x/a.txt", "same");
        MainWindow window; auto *permanent = window.findChild<QCheckBox *>("permanentDelete"); QVERIFY(permanent); QVERIFY(!permanent->isChecked());
        auto *tree = window.findChild<QTreeView *>("fileTree"); QVERIFY(tree); QCOMPARE(tree->selectionMode(), QAbstractItemView::ExtendedSelection);
        QVERIFY(!window.windowIcon().isNull());
        auto *progress = window.findChild<QProgressBar *>("taskProgress"); auto *execute = window.findChild<QPushButton *>("executeTask");
        QVERIFY(progress); QVERIFY(execute); window.show(); QCoreApplication::processEvents();
        QVERIFY(progress->mapTo(&window, QPoint()).y() > tree->mapTo(&window, QPoint()).y() + tree->height());
        QVERIFY(qAbs(progress->mapTo(&window, progress->rect().center()).y() - execute->mapTo(&window, execute->rect().center()).y()) <= 2);
        QSignalSpy ready(window.viewModel(), &ViewModel::scanReady); ScanOptions options; options.rootA = dir.path(); options.backend = Backend::Native;
        window.viewModel()->scan(options); QVERIFY(window.viewModel()->busy()); QVERIFY(window.windowTitle().contains(QStringLiteral("任务进行中"))); QVERIFY(ready.wait(15000));
        QVERIFY(!window.viewModel()->busy()); QVERIFY(window.viewModel()->canExecute()); QCOMPARE(window.viewModel()->model()->plan().size(), 1);
        QVERIFY(!window.windowTitle().contains(QStringLiteral("任务进行中")));
        window.viewModel()->invalidate(); QVERIFY(!window.viewModel()->canExecute());
    }
    void pairedModelSelectionAndPersistentRows() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX");
        const auto original = createFile(dir.path(), "item.txt", "same");
        const auto second = createFile(dir.path(), "nested/item.txt", "same"), third = createFile(dir.path(), "elsewhere/item.txt", "same");
        FileTreeModel source; ComparisonModel pairs(&source); QAbstractItemModelTester tester(&pairs, QAbstractItemModelTester::FailureReportingMode::QtTest);
        source.setResult(scan(dir.path())); QCOMPARE(pairs.pairCount(), 2); QCOMPARE(pairs.rowCount(), 1);
        QVERIFY(pairs.index(0, 0).data(FileTreeModel::FolderRole).toBool());
        const auto row = pairIndex(pairs, second); QVERIFY(row.isValid());
        QCOMPARE(row.data(FileTreeModel::PathRole).toString(), original);
        QCOMPARE(row.data().toString(), row.sibling(row.row(), ComparisonModel::CandidateName).data().toString());
        QCOMPARE(pairs.keeperChoices(row).size(), 3);
        QPersistentModelIndex persistent(row); QSignalSpy reset(&pairs, &QAbstractItemModel::modelReset);
        QVERIFY(pairs.setData(row.sibling(row.row(), ComparisonModel::Replacement), Qt::Unchecked, Qt::CheckStateRole));
        QCOMPARE(source.plan().size(), 1); QCOMPARE(reset.count(), 0); QVERIFY(persistent.isValid());
        pairs.sort(ComparisonModel::CandidateName, Qt::DescendingOrder); QVERIFY(persistent.isValid());
        QCOMPARE(persistent.sibling(persistent.row(), ComparisonModel::CandidateName).data(FileTreeModel::PathRole).toString(), second);
        const auto chosenFile = fileIndex(source, third).data(FileTreeModel::FileRole).toInt();
        QVERIFY(pairs.setData(persistent, chosenFile, Qt::EditRole)); QCOMPARE(pairs.pairCount(), 2); QVERIFY(!pairIndex(pairs, third).isValid());
        QCOMPARE(pairIndex(pairs, original).data(FileTreeModel::PathRole).toString(), third);
        QVERIFY(!pairIndex(pairs, original).data().toString().contains("SHA-256"));
        QVERIFY(pairIndex(pairs, original).data(Qt::ToolTipRole).toString().contains("SHA-256"));
    }
    void pairedDualDirectoryTargets() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX"); const auto a = dir.path() + "/A", b = dir.path() + "/B";
        const auto original = createFile(a, "item.txt", "same"), alternative = createFile(a, "other/item.txt", "same");
        const auto first = createFile(b, "one/item.txt", "same"), second = createFile(b, "two/item.txt", "same");
        createFile(b, "only.bin", "B only");
        FileTreeModel source; ComparisonModel pairs(&source); QAbstractItemModelTester tester(&pairs, QAbstractItemModelTester::FailureReportingMode::QtTest);
        source.setResult(scan(a, b)); QCOMPARE(pairs.pairCount(), 2);
        const auto root = pairs.index(0, 0); QCOMPARE(root.data(FileTreeModel::PathRole).toString(), cleanPath(a));
        QCOMPARE(root.sibling(0, ComparisonModel::CandidateName).data(FileTreeModel::PathRole).toString(), cleanPath(b));
        const auto row = pairIndex(pairs, first); QCOMPARE(row.data(FileTreeModel::PathRole).toString(), original); QCOMPARE(pairs.keeperChoices(row).size(), 2);
        const int candidateFile = fileIndex(source, first).data(FileTreeModel::FileRole).toInt(); QVERIFY(!pairs.setData(row, candidateFile, Qt::EditRole));
        const int alternateFile = fileIndex(source, alternative).data(FileTreeModel::FileRole).toInt(); QVERIFY(pairs.setData(row, alternateFile, Qt::EditRole));
        for (const auto &path : {first, second}) {
            const auto i = pairIndex(pairs, path); QCOMPARE(i.data(FileTreeModel::PathRole).toString(), alternative);
            QCOMPARE(pairs.sourceIndex(i, true).data(FileTreeModel::PathRole).toString(), alternative);
            QCOMPARE(pairs.sourceIndex(i, false).data(FileTreeModel::PathRole).toString(), path);
        }
        QCOMPARE(source.plan().size(), 2); for (const auto &op : source.plan()) QCOMPARE(op.target.path, alternative);
        pairs.sort(ComparisonModel::CandidateName, Qt::DescendingOrder); QVERIFY(pairs.index(0, 0).data(FileTreeModel::FolderRole).toBool());
    }
    void directoryModesAndFrozenComparisonRows() {
        QTemporaryDir dir(QDir::currentPath() + "/test-output/case-XXXXXX"); const auto a = dir.path() + "/A", b = dir.path() + "/B";
        for (int i = 0; i < 70; ++i) { const auto name = QString("file-%1.txt").arg(i, 3, 10, QChar('0')); createFile(a, name, "same"); createFile(b, "nested/" + name, "same"); }
        MainWindow window; window.show(); auto *mode = window.findChild<QComboBox *>("mode"); QVERIFY(mode);
        mode->setCurrentIndex(0); QCoreApplication::processEvents(); auto *panelA = window.findChild<QWidget *>("directoryA"), *panelB = window.findChild<QWidget *>("directoryB");
        QVERIFY(panelA->isVisible()); QVERIFY(!panelB->isVisible()); const int singleWidth = panelA->width();
        mode->setCurrentIndex(1); QCoreApplication::processEvents(); QVERIFY(panelB->isVisible()); QVERIFY(panelA->width() < singleWidth);
        QCOMPARE(panelA->mapTo(&window, QPoint()).y(), panelB->mapTo(&window, QPoint()).y());
        auto *pairs = window.findChild<ComparisonModel *>(); auto *view = window.findChild<ComparisonView *>(); QVERIFY(pairs); QVERIFY(view);
        window.viewModel()->model()->setResult(scan(a, b)); QCoreApplication::processEvents(); QCOMPARE(pairs->pairCount(), 70);
        auto *left = view->originalView(), *right = view->candidateView(); QCOMPARE(left->selectionModel(), right->selectionModel());
        auto *leftRoot = window.findChild<QTreeView *>("originalRootRow"), *rightRoot = window.findChild<QTreeView *>("candidateRootRow"); QVERIFY(leftRoot); QVERIFY(rightRoot);
        const auto root = pairs->index(0, 0); const auto pinned = leftRoot->visualRect(root); QVERIFY(!pinned.isEmpty());
        QVERIFY(leftRoot->viewport()->rect().contains(pinned)); QVERIFY(rightRoot->viewport()->rect().contains(rightRoot->visualRect(root.sibling(0, ComparisonModel::CandidateName))));
        QVERIFY(right->verticalScrollBar()->maximum() > 0); right->verticalScrollBar()->setValue(right->verticalScrollBar()->maximum());
        QCoreApplication::processEvents(); QCOMPARE(left->verticalScrollBar()->value(), right->verticalScrollBar()->value());
        QCOMPARE(leftRoot->visualRect(root), pinned); QVERIFY(root.data(FileTreeModel::FolderRole).toBool()); QCOMPARE(rightRoot->verticalScrollBar()->value(), 0);
        const auto leftTop = left->indexAt(QPoint(65, 10)), rightTop = right->indexAt(QPoint(65, 10)); QVERIFY(leftTop.isValid()); QVERIFY(rightTop.isValid());
        QCOMPARE(leftTop.data(FileTreeModel::GroupRole), rightTop.data(FileTreeModel::GroupRole)); QCOMPARE(leftTop.row(), rightTop.row());
        const auto last = pairIndex(*pairs, b + "/nested/file-069.txt"); QPersistentModelIndex stable(last); const int position = right->verticalScrollBar()->value();
        QVERIFY(pairs->setData(last.sibling(last.row(), ComparisonModel::Replacement), Qt::Unchecked, Qt::CheckStateRole));
        QCoreApplication::processEvents(); QVERIFY(stable.isValid()); QCOMPARE(right->verticalScrollBar()->value(), position);
        right->selectionModel()->select(stable, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows); right->setFocus(); QCoreApplication::processEvents();
        QCOMPARE(view->sourceSelection().first().data(FileTreeModel::PathRole).toString(), b + "/nested/file-069.txt");
        left->setFocus(); QCoreApplication::processEvents(); QCOMPARE(view->sourceSelection().first().data(FileTreeModel::PathRole).toString(), a + "/file-069.txt");
        auto *splitter = window.findChild<QSplitter *>("comparisonSplitter"); QVERIFY(splitter); QVERIFY(splitter->handle(1)->isEnabled());
        const auto sizes = splitter->sizes(); splitter->setSizes({sizes[0] + 70, sizes[1] - 70}); QVERIFY(splitter->sizes()[0] > sizes[0]);
        const auto folder = pairs->index(0, 0, root); left->collapse(folder); QVERIFY(!right->isExpanded(folder)); right->expand(folder); QVERIFY(left->isExpanded(folder));
    }
};
QTEST_MAIN(DupTests)
#include "test_dup.moc"
