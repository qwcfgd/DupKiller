#include "mainwindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
#include <QScrollBar>
#include <QTimer>
#include <QTreeView>

int main(int argc, char **argv) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("QtDupKiller"); QCoreApplication::setApplicationName("QtDupKiller");
    QCoreApplication::setApplicationVersion("1.0.0");
    app.setStyle("Fusion");
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/QtDupKiller.ico")));
    QCommandLineParser parser; parser.setApplicationDescription(QStringLiteral("Qt 5 / Qt 6 重复文件快捷方式整理工具"));
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"smoke-output", "Save a read-only UI smoke screenshot and exit.", "png"});
    parser.addOption({"scan-dir", "Scan a fixture directory during the UI smoke test (never executes replacements).", "directory"});
    parser.addOption({"scan-b", "Use dual-directory comparison with this fixture B directory.", "directory"});
    parser.addOption({"smoke-scroll", "Scroll the comparison to its bottom before saving the screenshot."});
    parser.process(app);
    if (parser.isSet("smoke-output")) QCoreApplication::setApplicationName("QtDupKillerPreview");
    dup::MainWindow window;
    if (parser.isSet("smoke-output")) window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    const QString output = parser.value("smoke-output");
    if (!output.isEmpty()) {
        QTimer::singleShot(60000, &app, [&app] { app.exit(3); });
        const bool scroll = parser.isSet("smoke-scroll");
        auto save = [&app, &window, output, scroll] { QTimer::singleShot(300, &window, [&app, &window, output, scroll] {
            if (scroll) { auto *tree = window.findChild<QTreeView *>("fileTree"); if (tree) tree->verticalScrollBar()->setValue(tree->verticalScrollBar()->maximum()); }
            QTimer::singleShot(100, &window, [&app, &window, output] { app.exit(window.grab().save(output) ? 0 : 2); });
        }); };
        if (parser.isSet("scan-dir")) { QObject::connect(window.viewModel(), &dup::ViewModel::scanReady, &window, save); if (parser.isSet("scan-b")) window.scanDirectories(parser.value("scan-dir"), parser.value("scan-b")); else window.scanDirectory(parser.value("scan-dir")); }
        else save();
    }
    return app.exec();
}
