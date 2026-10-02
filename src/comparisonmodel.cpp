#include "comparisonmodel.h"
#include <QApplication>
#include <QBrush>
#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QStyle>
#include <algorithm>

namespace dup {
ComparisonModel::ComparisonModel(FileTreeModel *source, QObject *parent)
    : QAbstractItemModel(parent), source(source), root(std::make_unique<Node>()) {
    connect(source, &QAbstractItemModel::modelReset, this, &ComparisonModel::rebuild);
    connect(source, &FileTreeModel::planChanged, this, &ComparisonModel::refreshPlan);
    rebuild();
}
ComparisonModel::Node *ComparisonModel::node(const QModelIndex &i) const { return i.isValid() ? static_cast<Node *>(i.internalPointer()) : root.get(); }
QModelIndex ComparisonModel::forNode(Node *n, int c) const { return n && n != root.get() ? createIndex(n->row, c, n) : QModelIndex(); }
QModelIndex ComparisonModel::index(int row, int column, const QModelIndex &parent) const {
    const auto *p = node(parent);
    if (row < 0 || column < 0 || column >= ColumnCount || row >= int(p->children.size()) || (parent.isValid() && parent.column() != 0)) return {};
    return createIndex(row, column, p->children[size_t(row)].get());
}
QModelIndex ComparisonModel::parent(const QModelIndex &i) const { return i.isValid() ? forNode(node(i)->parent) : QModelIndex(); }
int ComparisonModel::rowCount(const QModelIndex &p) const { return p.isValid() && p.column() != 0 ? 0 : int(node(p)->children.size()); }
QVariant ComparisonModel::headerData(int s, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole || s < 0 || s >= ColumnCount) return {};
    static const QStringList titles{QStringLiteral("原始文件 ▾"), QStringLiteral("所在目录"), QStringLiteral("大小"), QStringLiteral("修改时间"), QStringLiteral("类型"),
        QStringLiteral("待替换文件"), QStringLiteral("所在目录"), QStringLiteral("替换"), QStringLiteral("大小"), QStringLiteral("修改时间"), QStringLiteral("类型")};
    return titles[s];
}
QVariant ComparisonModel::data(const QModelIndex &i, int role) const {
    if (!i.isValid()) return {};
    const auto *n = node(i); const bool original = i.column() < CandidateName;
    if (role == FileTreeModel::FolderRole) return n->folder();
    if (role == FileTreeModel::GroupRole) return n->group;
    const auto &result = source->result();
    const QString path = n->folder() ? (original ? n->originalPath : n->candidatePath)
        : result.groups[n->group].files[original ? result.groups[n->group].keeper : n->candidate].path;
    if (role == FileTreeModel::PathRole) return path;
    if (n->folder()) {
        if (role == FileTreeModel::FileRole) return -1;
        if (role == Qt::ToolTipRole) return path;
        if (role == Qt::FontRole) { QFont f; f.setBold(true); return f; }
        if (role == Qt::DecorationRole && (i.column() == OriginalName || i.column() == CandidateName)) return QApplication::style()->standardIcon(QStyle::SP_DirIcon);
        if (role == Qt::BackgroundRole) return QBrush(QColor("#eef2f7"));
        if (role == Qt::DisplayRole) {
            if (i.column() == OriginalName || i.column() == CandidateName) {
                if (n->parent == root.get()) return original ? QStringLiteral("原件根目录") : QStringLiteral("副本根目录");
                if (original) {
                    const auto &originalRoot = result.options.rootA;
                    const QString relative = QDir(originalRoot).relativeFilePath(path);
                    return QStringLiteral("原件目录：") + (relative == "." ? QStringLiteral("根目录") : QDir::toNativeSeparators(relative));
                }
                return path.isEmpty() ? QStringLiteral("对应目录") : QFileInfo(path).fileName();
            }
            if (i.column() == OriginalDirectory || i.column() == CandidateDirectory) return n->parent == root.get()
                ? (path.isEmpty() ? QStringLiteral("请选择目录") : QDir::toNativeSeparators(path)) : QString();
            if (i.column() == OriginalType || i.column() == CandidateType) return QStringLiteral("文件夹");
        }
        return {};
    }
    const auto &g = result.groups[n->group]; const int file = original ? g.keeper : n->candidate; const auto &f = g.files[file];
    if (role == FileTreeModel::FileRole) return file;
    if (role == Qt::EditRole && i.column() == OriginalName) return g.keeper;
    if (role == Qt::ToolTipRole) return f.path + QStringLiteral("\n重复组：%1\nSHA-256：\n").arg(n->group + 1) + QString::fromLatin1(f.sha256.toHex());
    if (role == Qt::CheckStateRole && i.column() == Replacement) return g.replace[n->candidate] ? Qt::Checked : Qt::Unchecked;
    if (role == Qt::ForegroundRole) return QBrush(original ? QColor("#13755d") : g.replace[n->candidate] ? QColor("#a65410") : QColor("#7b8494"));
    if (role == Qt::DisplayRole) {
        switch (i.column()) {
        case OriginalName: case CandidateName: return f.name;
        case OriginalDirectory: case CandidateDirectory: { const auto dir = QDir(f.root).relativeFilePath(QFileInfo(f.path).absolutePath()); return dir == "." ? QStringLiteral("根目录") : QDir::toNativeSeparators(dir); }
        case OriginalSize: case CandidateSize: return readableSize(f.size);
        case OriginalModified: case CandidateModified: return f.modified.toString("yyyy-MM-dd HH:mm:ss");
        case OriginalType: case CandidateType: return QFileInfo(f.name).suffix().isEmpty() ? QStringLiteral("无扩展名") : QFileInfo(f.name).suffix().toUpper();
        case Replacement: return g.replace[n->candidate] ? QStringLiteral("→ .lnk") : QStringLiteral("跳过");
        default: break;
        }
    }
    return {};
}
Qt::ItemFlags ComparisonModel::flags(const QModelIndex &i) const {
    if (!i.isValid()) return Qt::NoItemFlags;
    auto flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (!node(i)->folder()) {
        if (i.column() == Replacement) flags |= Qt::ItemIsUserCheckable;
        if (i.column() == OriginalName && keeperChoices(i).size() > 1) flags |= Qt::ItemIsEditable;
    }
    return flags;
}
bool ComparisonModel::setData(const QModelIndex &i, const QVariant &value, int role) {
    if (!i.isValid() || node(i)->folder()) return false;
    const auto *n = node(i);
    if (i.column() == Replacement && role == Qt::CheckStateRole) {
        const auto sourceFile = source->indexForFile(n->group, n->candidate);
        return source->setData(sourceFile.sibling(sourceFile.row(), FileTreeModel::Replace), value, role);
    }
    if (i.column() == OriginalName && role == Qt::EditRole) {
        for (const auto &choice : keeperChoices(i)) if (choice.second == value.toInt()) {
            const auto sourceFile = source->indexForFile(n->group, choice.second);
            return source->setData(sourceFile.sibling(sourceFile.row(), FileTreeModel::Keep), Qt::Checked, Qt::CheckStateRole);
        }
    }
    return false;
}
QVector<QPair<QString, int>> ComparisonModel::keeperChoices(const QModelIndex &i) const {
    QVector<QPair<QString, int>> choices;
    if (!i.isValid() || node(i)->folder()) return choices;
    const auto &result = source->result(); const auto &group = result.groups[node(i)->group];
    for (int f = 0; f < group.files.size(); ++f) if (result.options.mode == Mode::Single || group.files[f].side == "A")
        choices.append({QDir::toNativeSeparators(group.files[f].path), f});
    return choices;
}
QModelIndex ComparisonModel::sourceIndex(const QModelIndex &i, bool original) const {
    if (!i.isValid()) return {};
    const auto *n = node(i);
    if (n->folder()) return source->indexForPath(original ? n->originalPath : n->candidatePath);
    return source->indexForFile(n->group, original ? source->result().groups[n->group].keeper : n->candidate);
}
void ComparisonModel::setPreviewRoots(ScanOptions options) { preview = std::move(options); if (source->result().groups.isEmpty()) rebuild(); }
void ComparisonModel::describeFolders(Node *n, const QString &originalRoot) {
    QString common;
    for (const auto &child : n->children) {
        if (child->folder()) describeFolders(child.get(), originalRoot);
        const QString path = child->folder() ? child->originalPath : QFileInfo(source->result().groups[child->group].files[source->result().groups[child->group].keeper].path).absolutePath();
        if (common.isEmpty()) common = path;
        else while (!withinRoot(path, common) && !samePath(common, originalRoot)) {
            const auto parent = QFileInfo(common).absolutePath(); if (parent == common) break; common = parent;
        }
    }
    n->originalPath = n->parent == root.get() || common.isEmpty() ? originalRoot : common;
}
void ComparisonModel::rebuild() {
    beginResetModel(); root = std::make_unique<Node>(); pairs = 0; keepers.clear();
    const auto &result = source->result(); const auto options = result.options.rootA.isEmpty() ? preview : result.options;
    auto top = std::make_unique<Node>(); top->parent = root.get(); top->originalPath = options.rootA;
    top->candidatePath = options.mode == Mode::Dual ? options.rootB : options.rootA;
    auto *base = top.get(); root->children.push_back(std::move(top));
    auto folder = [](Node *parent, const QString &path) {
        for (const auto &child : parent->children) if (child->folder() && child->candidatePath == path) return child.get();
        auto item = std::make_unique<Node>(); item->parent = parent; item->candidatePath = path;
        auto *out = item.get(); parent->children.push_back(std::move(item)); return out;
    };
    for (int g = 0; g < result.groups.size(); ++g) {
        const auto &group = result.groups[g]; keepers.append(group.keeper); if (group.keeper < 0) continue;
        for (int f = 0; f < group.files.size(); ++f) {
            if (f == group.keeper || (result.options.mode == Mode::Dual && group.files[f].side != "B")) continue;
            const auto &file = group.files[f]; auto *p = base;
            QString cumulative = file.root; const auto parts = QDir(file.root).relativeFilePath(file.path).split('/');
            for (int part = 0; part + 1 < parts.size(); ++part) { cumulative = QDir(cumulative).filePath(parts[part]); p = folder(p, cumulative); }
            auto item = std::make_unique<Node>(); item->parent = p; item->group = g; item->candidate = f; item->candidatePath = file.path;
            item->size = file.size; item->modified = file.modified; item->originalModified = group.files[group.keeper].modified;
            p->children.push_back(std::move(item));
            ++pairs;
            for (auto *ancestor = p; ancestor; ancestor = ancestor->parent) {
                ancestor->size += file.size; ancestor->modified = qMax(ancestor->modified, file.modified);
                ancestor->originalModified = qMax(ancestor->originalModified, group.files[group.keeper].modified);
            }
        }
    }
    describeFolders(base, options.rootA); sortNode(root.get()); endResetModel();
}
void ComparisonModel::refreshPlan() {
    const auto &groups = source->result().groups;
    if (keepers.size() != groups.size()) { rebuild(); return; }
    for (int g = 0; g < groups.size(); ++g) if (keepers[g] != groups[g].keeper) { rebuild(); return; }
    // Replacement checkboxes do not change row identities or the scroll position.
    std::function<void(Node *)> notify = [&](Node *p) {
        if (!p->children.empty()) emit dataChanged(forNode(p->children.front().get()), forNode(p->children.back().get(), ColumnCount - 1));
        for (const auto &child : p->children) notify(child.get());
    };
    notify(root.get());
}
void ComparisonModel::sortNode(Node *p) {
    QCollator collator; collator.setNumericMode(true); collator.setCaseSensitivity(Qt::CaseSensitive);
    std::stable_sort(p->children.begin(), p->children.end(), [&](const auto &a, const auto &b) {
        if (a->folder() != b->folder()) return a->folder();
        const bool original = sortColumn < CandidateName;
        auto path = [&](const Node *n) { return n->folder() ? (original ? n->originalPath : n->candidatePath)
            : source->result().groups[n->group].files[original ? source->result().groups[n->group].keeper : n->candidate].path; };
        int compare = 0;
        if (sortColumn == OriginalDirectory || sortColumn == CandidateDirectory) compare = collator.compare(QFileInfo(path(a.get())).absolutePath(), QFileInfo(path(b.get())).absolutePath());
        else if (sortColumn == OriginalSize || sortColumn == CandidateSize) compare = a->size < b->size ? -1 : a->size > b->size ? 1 : 0;
        else if (sortColumn == OriginalModified || sortColumn == CandidateModified) {
            const auto x = original ? a->originalModified : a->modified, y = original ? b->originalModified : b->modified;
            compare = x < y ? -1 : x > y ? 1 : 0;
        } else if (sortColumn == OriginalType || sortColumn == CandidateType) compare = collator.compare(QFileInfo(path(a.get())).suffix(), QFileInfo(path(b.get())).suffix());
        else if (sortColumn == Replacement && !a->folder()) compare = int(source->result().groups[a->group].replace[a->candidate]) - int(source->result().groups[b->group].replace[b->candidate]);
        if (!compare) compare = collator.compare(QFileInfo(path(a.get())).fileName(), QFileInfo(path(b.get())).fileName());
        if (!compare) compare = QString::compare(a->candidatePath, b->candidatePath);
        return sortOrder == Qt::AscendingOrder ? compare < 0 : compare > 0;
    });
    for (size_t row = 0; row < p->children.size(); ++row) { p->children[row]->row = int(row); sortNode(p->children[row].get()); }
}
void ComparisonModel::sort(int column, Qt::SortOrder order) {
    if (column < 0 || column >= ColumnCount) return;
    sortColumn = column; sortOrder = order;
    const auto before = persistentIndexList(); emit layoutAboutToBeChanged(); sortNode(root.get());
    QModelIndexList after; for (const auto &i : before) after.append(forNode(node(i), i.column()));
    changePersistentIndexList(before, after); emit layoutChanged();
}
}
