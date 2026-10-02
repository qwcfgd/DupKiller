#pragma once
#include "domain.h"
#include <QAbstractItemModel>
#include <QSet>
#include <QHash>
#include <memory>
#include <vector>

namespace dup {
class FileTreeModel final : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Column { Name, Keep, Replace, Size, Modified, Type, Target, ColumnCount };
    enum Role { GroupRole = Qt::UserRole + 1, FileRole, PathRole, FolderRole };
    explicit FileTreeModel(QObject *parent = nullptr);
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex & = {}) const override { return ColumnCount; }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    void sort(int column, Qt::SortOrder order) override;
    void setResult(ScanResult result);
    void clear();
    const ScanResult &result() const { return scan; }
    QVector<Operation> plan() const;
    quint64 saving() const;
    int operationCount() const { return selectedCount; }
    QModelIndex indexForFile(int group, int file) const;
    QModelIndex indexForPath(const QString &path) const;
    QString chooseSelected(const QModelIndexList &selected);
    void applyRule(const QModelIndexList &selected, KeepRule rule);
    void preferFolders(const QModelIndexList &selected);
    void setReplacement(const QModelIndexList &selected, bool enabled);
    void resetDefaults();
signals:
    void planChanged();
    void groupsChanged(const QVector<int> &groups);
private:
    struct Node {
        QString name, path;
        Node *parent = nullptr;
        int row = 0, group = -1, file = -1;
        quint64 size = 0;
        QDateTime modified;
        std::vector<std::unique_ptr<Node>> children;
        bool folder() const { return file < 0; }
    };
    std::unique_ptr<Node> root;
    ScanResult scan;
    QVector<QVector<Node *>> groupNodes;
    QHash<QString, Node *> pathNodes;
    QVector<quint64> groupSavings;
    QVector<int> groupCounts;
    quint64 selectedBytes = 0;
    int selectedCount = 0;
    int sortColumn = Name;
    Qt::SortOrder sortOrder = Qt::AscendingOrder;
    Node *node(const QModelIndex &index) const;
    QModelIndex forNode(Node *node, int column = 0) const;
    void notifyGroup(int group);
    void notifyChanges(const QSet<int> &groups);
    void updateTotals(int group);
    QSet<int> selectedGroups(const QModelIndexList &selected) const;
    void collect(Node *node, QSet<int> &groups) const;
    void sortNode(Node *node);
};
}
