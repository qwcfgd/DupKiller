#include "comparisonview.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollBar>
#include <QTreeView>
#include <QTextStream>

using namespace dup;
// Synthetic records only: no photo directory is opened or modified.
int main(int argc, char **argv) {
    QApplication app(argc, argv); app.setStyle("Fusion");
    const int copies = argc > 1 ? QString::fromLocal8Bit(argv[1]).toInt() : 3000;
    if (copies < 1 || copies > 100000) return 2;
    ScanResult result; result.options.rootA = "C:/DupKillerBenchmark"; DuplicateGroup group;
    for (int f = 0; f <= copies; ++f) {
        FileRecord file; file.root = result.options.rootA; file.name = "photo.jpg"; file.side = "A";
        file.path = file.root + (f ? QString("/copies/%1/").arg(f, 5, 10, QChar('0')) : "/") + file.name;
        file.depth = f ? 2 : 0; file.size = 1024; file.sha256 = QByteArray(32, 'a'); file.modified = QDateTime::currentDateTime();
        group.files.append(file);
    }
    setKeeper(group, Mode::Single, 0); result.groups.append(group);
    FileTreeModel source; ComparisonModel pairs(&source); QElapsedTimer clock; clock.start(); source.setResult(result);
    QJsonObject report{{"qt", QString::fromLatin1(qVersion())}, {"copies", copies}, {"modelBuildMs", double(clock.elapsed())}};
    clock.restart(); ComparisonView view(&pairs); view.setAttribute(Qt::WA_DontShowOnScreen); view.resize(1380, 600); view.show(); QCoreApplication::processEvents();
    report["viewBuildMs"] = double(clock.elapsed());
    clock.restart();
    for (int step = 0; step < 10; ++step) {
        view.candidateView()->verticalScrollBar()->setValue(step * view.candidateView()->verticalScrollBar()->maximum() / 9);
        QCoreApplication::processEvents(); view.grab();
    }
    report["tenScrollAndPaintMs"] = double(clock.elapsed());
    const auto file = source.indexForFile(0, copies); clock.restart(); source.setData(file.sibling(file.row(), FileTreeModel::Replace), Qt::Unchecked, Qt::CheckStateRole); QCoreApplication::processEvents(); view.grab();
    report["checkboxAndPaintMs"] = double(clock.elapsed());
    const auto output = QJsonDocument(report).toJson(); QTextStream(stdout) << output;
    if (argc > 2) { QFile file(QString::fromLocal8Bit(argv[2])); if (!file.open(QIODevice::WriteOnly) || file.write(output) != output.size()) return 2; }
    return 0;
}
