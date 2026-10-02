#include "mainwindow.h"
#include "comparisonview.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace dup {
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), vm(this) {
    setWindowTitle(QStringLiteral("Qt DupKiller · 重复文件整理")); resize(1380, 900); setMinimumSize(1080, 700);
    setWindowIcon(QIcon(QStringLiteral(":/icons/QtDupKiller.ico")));
    setStyleSheet(QStringLiteral(
        "QMainWindow{background:#f4f6fa;} QWidget{font-family:'Microsoft YaHei UI';font-size:10pt;color:#253247;}"
        "QLineEdit,QComboBox{background:white;border:1px solid #d7deea;border-radius:5px;padding:6px;}"
        "QPushButton{background:white;border:1px solid #ced7e4;border-radius:5px;padding:7px 12px;}"
        "QPushButton:hover{background:#edf4f1;border-color:#6a9d8e;} QPushButton:disabled{color:#9ca7b4;background:#eef1f5;}"
        "QPushButton#primary,QPushButton#executeTask{background:#13755d;color:white;border:1px solid #13755d;font-weight:bold;}"
        "QPushButton#primary:disabled,QPushButton#executeTask:disabled{background:#aecac1;border-color:#aecac1;}"
        "QTreeView{background:white;alternate-background-color:#f8fafc;border:1px solid #d7deea;border-radius:6px;}"
        "QTreeView::item{height:29px;} QTreeView::item:selected{background:#dceee8;color:#123d32;}"
        "QHeaderView::section{background:#eef2f7;padding:8px;border:0;border-right:1px solid #d7deea;font-weight:bold;}"
        "QPlainTextEdit{background:#fff;border:1px solid #d7deea;border-radius:5px;font-size:9pt;}"
        "QProgressBar{border:1px solid #d7deea;border-radius:4px;background:white;height:14px;text-align:center;}"
        "QProgressBar::chunk{background:#27856c;} QLabel#title{font-size:22pt;font-weight:bold;color:#174b3b;}"
        "QLabel#muted{color:#647388;} QCheckBox{spacing:7px;}"));
    auto *central = new QWidget(this); setCentralWidget(central);
    auto *layout = new QVBoxLayout(central); layout->setContentsMargins(22, 18, 22, 18); layout->setSpacing(10);
    auto *title = new QLabel(QStringLiteral("重复文件整理")); title->setObjectName("title"); layout->addWidget(title);
    auto *intro = new QLabel(QStringLiteral("同名 · 同大小 · 同 SHA-256   |   保留原件，让副本成为指向原件的快捷方式")); intro->setObjectName("muted"); layout->addWidget(intro);
    auto *settings = new QHBoxLayout;
    mode = new QComboBox; mode->addItems({QStringLiteral("单目录模式"), QStringLiteral("双目录模式：B → A")}); mode->setObjectName("mode");
    backend = new QComboBox; backend->addItems({QStringLiteral("自动：NTFS 加速 / 原生后备"), QStringLiteral("Windows 原生遍历")});
    settings->addWidget(new QLabel(QStringLiteral("模式"))); settings->addWidget(mode); settings->addSpacing(18);
    settings->addWidget(new QLabel(QStringLiteral("扫描方式"))); settings->addWidget(backend); settings->addStretch();
    auto *version = new QLabel(QStringLiteral("Qt %1 · CMake · x64").arg(QString::fromLatin1(qVersion()))); version->setObjectName("muted"); settings->addWidget(version); layout->addLayout(settings);
    editingControls << mode << backend;
    auto *pathRow = new QHBoxLayout; pathRow->setSpacing(16);
    auto addPath = [&](const QString &label, QLineEdit *&edit, QLabel *&caption, QPushButton *&browse) {
        auto *panel = new QWidget; auto *row = new QHBoxLayout(panel); row->setContentsMargins(0, 0, 0, 0); caption = new QLabel(label); caption->setFixedWidth(60);
        edit = new QLineEdit; edit->setPlaceholderText(QStringLiteral("选择要递归扫描的目录")); browse = new QPushButton(QStringLiteral("选择目录…"));
        row->addWidget(caption); row->addWidget(edit, 1); row->addWidget(browse); pathRow->addWidget(panel, 1);
        editingControls << edit << browse;
        connect(browse, &QPushButton::clicked, this, [this, edit] { const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("选择扫描目录"), edit->text()); if (!path.isEmpty()) edit->setText(QDir::toNativeSeparators(path)); });
        return panel;
    };
    QLabel *captionA, *captionB; QPushButton *browseA, *browseB;
    auto *panelA = addPath(QStringLiteral("目录"), pathA, captionA, browseA); auto *panelB = addPath(QStringLiteral("目录 B"), pathB, captionB, browseB);
    panelA->setObjectName("directoryA"); panelB->setObjectName("directoryB"); layout->addLayout(pathRow);
    pathA->setObjectName("pathA"); pathB->setObjectName("pathB");
    panelB->setVisible(false); pathB->setEnabled(false); browseB->setEnabled(false);
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, browseB, panelB, captionA](int index) {
        panelB->setVisible(index == 1); captionA->setText(index == 1 ? QStringLiteral("目录 A") : QStringLiteral("目录"));
        pathB->setEnabled(index == 1 && !vm.busy()); browseB->setEnabled(index == 1 && !vm.busy());
    });
    auto *scanRow = new QHBoxLayout;
    scanButton = new QPushButton(QStringLiteral("扫描并生成清单")); scanButton->setObjectName("primary");
    cancelButton = new QPushButton(QStringLiteral("取消")); cancelButton->setEnabled(false);
    phase = new QLabel(QStringLiteral("就绪。扫描不会修改文件。")); phase->setObjectName("muted");
    phase->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    progress = new QProgressBar; progress->setObjectName("taskProgress"); progress->setRange(0, 1000); progress->setValue(0); progress->setMinimumWidth(220); progress->setTextVisible(true);
    scanRow->addWidget(scanButton); scanRow->addWidget(cancelButton); scanRow->addWidget(phase, 1); scanRow->addWidget(progress);
    connect(scanButton, &QPushButton::clicked, this, &MainWindow::startScan); connect(cancelButton, &QPushButton::clicked, &vm, &ViewModel::cancel);
    auto *listTitle = new QHBoxLayout;
    auto *caption = new QLabel(QStringLiteral("保留及替换清单")); QFont captionFont = caption->font(); captionFont.setBold(true); caption->setFont(captionFont);
    listTitle->addWidget(caption); listTitle->addStretch();
    permanent = new QCheckBox(QStringLiteral("永久删除副本（默认关闭）")); permanent->setChecked(false); permanent->setObjectName("permanentDelete");
    permanent->setToolTip(QStringLiteral("关闭：将副本送入回收站。开启：验证快捷方式后永久删除副本。"));
    listTitle->addWidget(permanent); editingControls << permanent; layout->addLayout(listTitle);
    auto *toolbar = new QHBoxLayout;
    auto *choose = new QPushButton(QStringLiteral("选中文件设为保留")); auto *prefer = new QPushButton(QStringLiteral("优先保留选中目录"));
    rule = new QComboBox; rule->setObjectName("keepRule"); rule->addItems({QStringLiteral("路径层级最浅"), QStringLiteral("修改时间最早"), QStringLiteral("修改时间最新"), QStringLiteral("优先替换日期说明目录")});
    auto *apply = new QPushButton(QStringLiteral("批量应用规则"));
    auto *defaults = new QPushButton(QStringLiteral("默认")); defaults->setObjectName("resetDefaults"); defaults->setToolTip(QStringLiteral("所有组恢复最浅路径原件及副本替换勾选，关闭永久删除。"));
    listTitle->insertWidget(1, defaults);
    toolbar->addWidget(choose); toolbar->addWidget(prefer); toolbar->addWidget(rule); toolbar->addWidget(apply); toolbar->addStretch();
    displayMode = new QComboBox; displayMode->setObjectName("displayMode"); displayMode->addItems({QStringLiteral("根目录"), QStringLiteral("子目录"), QStringLiteral("文件")}); displayMode->setCurrentIndex(2);
    displayMode->setToolTip(QStringLiteral("根目录：只显示固定根行；子目录：所有子目录；文件：按目录层级显示重复文件。显示方式不改变执行清单。"));
    listTitle->insertWidget(2, new QLabel(QStringLiteral("显示"))); listTitle->insertWidget(3, displayMode);
    sortKey = new QComboBox; sortKey->addItems({QStringLiteral("文件名"), QStringLiteral("修改时间"), QStringLiteral("文件类型"), QStringLiteral("大小")});
    sortDirection = new QComboBox; sortDirection->addItems({QStringLiteral("升序 ↑"), QStringLiteral("降序 ↓")});
    toolbar->addWidget(new QLabel(QStringLiteral("排序"))); toolbar->addWidget(sortKey); toolbar->addWidget(sortDirection); layout->addLayout(toolbar);
    editingControls << defaults << choose << prefer << rule << apply << displayMode << sortKey << sortDirection;
    auto *help = new QLabel(QStringLiteral("左右同一行即同一组匹配。点击左侧原件可切换保留目标；Ctrl / Shift 多选后可批量处理。根目录行固定，SHA-256 仅悬停显示。"));
    help->setObjectName("muted"); layout->addWidget(help);
    auto *splitter = new QSplitter(Qt::Vertical);
    comparison = new ComparisonModel(vm.model(), this); comparisonView = new ComparisonView(comparison); tree = comparisonView->candidateView();
    splitter->addWidget(comparisonView);
    log = new QPlainTextEdit; log->setObjectName("operationLog"); log->setReadOnly(true); log->setMaximumBlockCount(1500); log->setPlaceholderText(QStringLiteral("扫描方式、跳过原因和执行日志将在此显示。")); splitter->addWidget(log);
    splitter->setSizes({450, 120}); layout->addWidget(splitter, 1);
    connect(defaults, &QPushButton::clicked, this, [this] { permanent->setChecked(false); rule->setCurrentIndex(0); vm.model()->resetDefaults(); });
    connect(displayMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) { comparison->setDisplayMode(static_cast<ComparisonModel::DisplayMode>(index)); });
    connect(choose, &QPushButton::clicked, this, [this] { const QString error = vm.model()->chooseSelected(selection()); if (!error.isEmpty()) QMessageBox::information(this, QStringLiteral("选择保留文件"), error); });
    connect(prefer, &QPushButton::clicked, this, [this] { vm.model()->preferFolders(selection()); });
    connect(apply, &QPushButton::clicked, this, [this] { vm.model()->applyRule(selection(), static_cast<KeepRule>(rule->currentIndex())); });
    auto updateSort = [this] { const int columns[] = {ComparisonModel::CandidateName, ComparisonModel::CandidateModified, ComparisonModel::CandidateType, ComparisonModel::CandidateSize}; comparisonView->setSort(columns[sortKey->currentIndex()], sortDirection->currentIndex() == 0 ? Qt::AscendingOrder : Qt::DescendingOrder); };
    connect(sortKey, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateSort);
    connect(sortDirection, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateSort);
    connect(comparisonView, &ComparisonView::contextRequested, this, [this](QTreeView *view, const QPoint &pos) {
        const auto index = view->indexAt(pos); if (!index.isValid()) return;
        if (!view->selectionModel()->isSelected(index)) view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QMenu menu;
        auto *keep = menu.addAction(QStringLiteral("选中文件设为保留")); auto *replace = menu.addAction(QStringLiteral("选中副本参与替换")); auto *skip = menu.addAction(QStringLiteral("跳过选中副本"));
        keep->setEnabled(!vm.busy()); replace->setEnabled(!vm.busy()); skip->setEnabled(!vm.busy()); menu.addSeparator();
        auto *open = menu.addAction(QStringLiteral("打开所在目录"));
        const auto action = menu.exec(view->viewport()->mapToGlobal(pos));
        if (action == keep) { const QString error = vm.model()->chooseSelected(selection()); if (!error.isEmpty()) QMessageBox::information(this, QStringLiteral("选择保留文件"), error); }
        else if (action == replace) vm.model()->setReplacement(selection(true), true);
        else if (action == skip) vm.model()->setReplacement(selection(true), false);
        else if (action == open) { const QString p = index.data(FileTreeModel::PathRole).toString(); QDesktopServices::openUrl(QUrl::fromLocalFile(index.data(FileTreeModel::FolderRole).toBool() ? p : QFileInfo(p).absolutePath())); }
    });
    auto *bottom = new QHBoxLayout;
    summary = new QLabel(QStringLiteral("等待扫描。只显示满足三个条件的重复文件。")); summary->setWordWrap(true); bottom->addWidget(summary, 1);
    auto *recover = new QPushButton(QStringLiteral("从日志恢复…")); auto *logs = new QPushButton(QStringLiteral("日志目录"));
    recover->setToolTip(QStringLiteral("恢复未完成的暂存事务，或将已回收的副本恢复到原路径。永久删除且已完成的副本无法恢复。"));
    executeButton = new QPushButton(QStringLiteral("开始任务：执行清单")); executeButton->setObjectName("executeTask"); executeButton->setEnabled(false);
    bottom->addWidget(recover); bottom->addWidget(logs); layout->addLayout(bottom); editingControls << recover;
    scanRow->addWidget(executeButton); layout->addLayout(scanRow);
    connect(recover, &QPushButton::clicked, this, [this] {
        const QString journal = QFileDialog::getOpenFileName(this, QStringLiteral("选择操作日志，恢复暂存文件或回收站副本"), vm.logsDirectory(), "QtDupKiller logs (*.jsonl)");
        if (!journal.isEmpty()) vm.recover(journal);
    });
    connect(logs, &QPushButton::clicked, this, [this] { QDir().mkpath(vm.logsDirectory()); QDesktopServices::openUrl(QUrl::fromLocalFile(vm.logsDirectory())); });
    connect(executeButton, &QPushButton::clicked, this, [this] {
        const QString deletion = permanent->isChecked() ? QStringLiteral("永久删除副本，无法通过回收站撤销") : QStringLiteral("将副本送入回收站");
        const QString text = QStringLiteral("将替换 %1 份副本（%2），%3。\n原件将保留；每份副本会被同目录的“原文件名.lnk”替代。\n\n执行前会重新校验文件。确认执行？")
            .arg(vm.model()->operationCount()).arg(readableSize(vm.model()->saving())).arg(deletion);
        if (QMessageBox::question(this, QStringLiteral("确认执行清单"), text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) vm.execute(permanent->isChecked());
    });
    connect(&vm, &ViewModel::message, log, &QPlainTextEdit::appendPlainText);
    connect(&vm, &ViewModel::summaryChanged, this, [this](const QString &s) { summary->setText(s); updateActions(); });
    connect(&vm, &ViewModel::busyChanged, this, [this, browseB](bool busy) {
        for (auto *control : editingControls) control->setEnabled(!busy);
        pathB->setEnabled(!busy && mode->currentIndex() == 1); browseB->setEnabled(!busy && mode->currentIndex() == 1);
        comparisonView->setEnabled(!busy); scanButton->setEnabled(!busy); cancelButton->setEnabled(busy); updateActions();
        setWindowTitle(busy ? QStringLiteral("Qt DupKiller · 任务进行中") : QStringLiteral("Qt DupKiller · 重复文件整理"));
        if (busy) { progress->setRange(0, 0); phase->setText(QStringLiteral("任务启动中…")); }
        if (!busy) { progress->setRange(0, 1000); progress->setValue(1000); if (closing) close(); }
    });
    connect(&vm, &ViewModel::progressChanged, this, [this](const QString &p, quint64 done, quint64 total) {
        phase->setText(total ? p + QStringLiteral("  %1 / %2").arg(done).arg(total) : p + QStringLiteral("  %1").arg(done));
        if (!total) progress->setRange(0, 0);
        else { progress->setRange(0, 1000); progress->setValue(int(qMin(1.0, double(done) / double(total)) * 1000)); }
    });
    QSettings settingsStore; pathA->setText(settingsStore.value("rootA").toString()); pathB->setText(settingsStore.value("rootB").toString());
    mode->setCurrentIndex(settingsStore.value("mode", 0).toInt() == 1 ? 1 : 0);
    connect(pathA, &QLineEdit::textChanged, &vm, &ViewModel::invalidate);
    connect(pathB, &QLineEdit::textChanged, &vm, &ViewModel::invalidate);
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), &vm, &ViewModel::invalidate);
    connect(backend, QOverload<int>::of(&QComboBox::currentIndexChanged), &vm, &ViewModel::invalidate);
    auto previewRoots = [this] { ScanOptions options; options.mode = mode->currentIndex() == 1 ? Mode::Dual : Mode::Single; options.rootA = pathA->text(); options.rootB = pathB->text(); comparison->setPreviewRoots(options); };
    connect(pathA, &QLineEdit::textChanged, this, previewRoots); connect(pathB, &QLineEdit::textChanged, this, previewRoots);
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, previewRoots); previewRoots();
}
QModelIndexList MainWindow::selection(bool candidates) const { return comparisonView->sourceSelection(candidates); }
void MainWindow::updateActions() { executeButton->setEnabled(vm.canExecute()); }
void MainWindow::startScan() {
    ScanOptions options; options.mode = mode->currentIndex() == 0 ? Mode::Single : Mode::Dual;
    options.backend = backend->currentIndex() == 0 ? Backend::Auto : Backend::Native; options.rootA = pathA->text(); options.rootB = pathB->text();
    QString error = validateRoots(options);
    if (!error.isEmpty()) { QMessageBox::warning(this, QStringLiteral("目录无效"), error); return; }
    QSettings settings; settings.setValue("rootA", pathA->text()); settings.setValue("rootB", pathB->text()); settings.setValue("mode", mode->currentIndex());
    permanent->setChecked(false); log->clear(); vm.scan(options);
}
void MainWindow::scanDirectory(const QString &path) { pathA->setText(QDir::toNativeSeparators(path)); mode->setCurrentIndex(0); startScan(); }
void MainWindow::scanDirectories(const QString &a, const QString &b) { pathA->setText(QDir::toNativeSeparators(a)); pathB->setText(QDir::toNativeSeparators(b)); mode->setCurrentIndex(1); startScan(); }
void MainWindow::closeEvent(QCloseEvent *event) {
    if (vm.busy()) {
        if (QMessageBox::question(this, QStringLiteral("任务正在运行"), QStringLiteral("取消后续操作，并在当前文件处理结束后退出？"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) { closing = true; vm.cancel(); }
        event->ignore(); return;
    }
    event->accept();
}
}
