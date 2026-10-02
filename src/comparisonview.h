#pragma once
#include "comparisonmodel.h"
#include <QWidget>

class QSplitter;
class QTreeView;
namespace dup {
class ComparisonView final : public QWidget {
    Q_OBJECT
public:
    explicit ComparisonView(ComparisonModel *model, QWidget *parent = nullptr);
    QTreeView *originalView() const { return original; }
    QTreeView *candidateView() const { return candidate; }
    QModelIndexList sourceSelection(bool candidates = false) const;
    void setSort(int column, Qt::SortOrder order);
signals:
    void contextRequested(QTreeView *view, const QPoint &position);
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    ComparisonModel *model;
    QTreeView *original, *candidate, *originalRoot, *candidateRoot;
    QSplitter *splitter;
    bool selectingOriginal = false, resizing = false;
    bool heightUpdateQueued = false;
    void bindPane(QTreeView *body, QTreeView *frozen, bool left);
    void restoreTree();
    void updateFrozenHeights();
};
}
