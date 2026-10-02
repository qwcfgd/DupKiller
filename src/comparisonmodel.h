#pragma once
#include "filetreemodel.h"

namespace dup {
// One candidate per row, paired with its group's current retained original.
// The hierarchy follows candidate directories so both views share exact rows.
class ComparisonModel final : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Column { OriginalName, OriginalDirectory, OriginalSize, OriginalModified, OriginalType,
                  CandidateName, CandidateDirectory, Replacement, CandidateSize, CandidateModified, CandidateType, ColumnCount };
    explicit ComparisonModel(FileTreeModel *source, QObject *parent = nullptr);
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &index) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex & = {}) const override { return ColumnCount; }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    void sort(int column, Qt::SortOrder order) override;
    void setPreviewRoots(ScanOptions options);
    QModelIndex sourceIndex(const QModelIndex &index, bool original) const;
    QVector<QPair<QString, int>> keeperChoices(const QModelIndex &index) const;
    int pairCount() const { return pairs; }
private:
    struct Node {
        Node *parent = nullptr;
        int row = 0, group = -1, candidate = -1;
        QString candidatePath, originalPath;
        quint64 size = 0;
        QDateTime modified, originalModified;
        std::vector<std::unique_ptr<Node>> children;
        bool folder() const { return candidate < 0; }
    };
    FileTreeModel *source;
    ScanOptions preview;
    std::unique_ptr<Node> root;
    int sortColumn = CandidateName;
    Qt::SortOrder sortOrder = Qt::AscendingOrder;
    QVector<int> keepers;
    int pairs = 0;
    Node *node(const QModelIndex &index) const;
    QModelIndex forNode(Node *node, int column = 0) const;
    void rebuild();
    void refreshPlan();
    void sortNode(Node *node);
    void describeFolders(Node *node, const QString &originalRoot);
};
}
