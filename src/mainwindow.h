#pragma once
#include "viewmodel.h"
#include <QMainWindow>
class QComboBox;
class QLineEdit;
class QCheckBox;
class QTreeView;
class QPushButton;
class QPlainTextEdit;
class QLabel;
class QProgressBar;
namespace dup {
class ComparisonModel;
class ComparisonView;
class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ViewModel *viewModel() { return &vm; }
    void scanDirectory(const QString &path);
    void scanDirectories(const QString &a, const QString &b);
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    ViewModel vm;
    QComboBox *mode, *backend, *rule, *sortKey, *sortDirection;
    QLineEdit *pathA, *pathB;
    QTreeView *tree;
    ComparisonModel *comparison;
    ComparisonView *comparisonView;
    QCheckBox *permanent;
    QPushButton *scanButton, *executeButton, *cancelButton;
    QPlainTextEdit *log;
    QLabel *summary, *phase;
    QProgressBar *progress;
    QVector<QWidget *> editingControls;
    bool closing = false;
    void startScan();
    void updateActions();
    QModelIndexList selection(bool candidates = false) const;
};
}
