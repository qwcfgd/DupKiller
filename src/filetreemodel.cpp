#include "filetreemodel.h"
#include <QBrush>
#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <algorithm>

namespace dup {
FileTreeModel::FileTreeModel(QObject *parent) : QAbstractItemModel(parent), root(std::make_unique<Node>()) {}
FileTreeModel::Node *FileTreeModel::node(const QModelIndex &i) const { return i.isValid() ? static_cast<Node *>(i.internalPointer()) : root.get(); }
QModelIndex FileTreeModel::forNode(Node *n, int c) const { return n && n != root.get() ? createIndex(n->row, c, n) : QModelIndex(); }
QModelIndex FileTreeModel::index(int row, int column, const QModelIndex &parent) const {
    Node *p = node(parent);
    if (row < 0 || column < 0 || column >= ColumnCount || row >= int(p->children.size()) || (parent.isValid() && parent.column() != 0)) return {};
    return createIndex(row, column, p->children[size_t(row)].get());
}
QModelIndex FileTreeModel::parent(const QModelIndex &i) const { return i.isValid() ? forNode(node(i)->parent) : QModelIndex(); }
int FileTreeModel::rowCount(const QModelIndex &p) const { return p.isValid() && p.column() != 0 ? 0 : int(node(p)->children.size()); }
QVariant FileTreeModel::headerData(int s, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole || s < 0 || s >= ColumnCount) return {};
    static const QStringList titles{QStringLiteral("目录 / 文件名"), QStringLiteral("保留原件"), QStringLiteral("替换副本"),
        QStringLiteral("大小"), QStringLiteral("修改时间"), QStringLiteral("文件类型"), QStringLiteral("快捷方式目标")};
    return titles[s];
}
QVariant FileTreeModel::data(const QModelIndex &i, int role) const {
    if (!i.isValid()) return {};
    const Node *n = node(i);
    if (role == GroupRole) return n->group;
    if (role == FileRole) return n->file;
    if (role == PathRole) return n->path;
    if (role == FolderRole) return n->folder();
    if (n->folder()) {
        if (role == Qt::DisplayRole) {
            if (i.column() == Name) return n->name;
            if (i.column() == Size) return readableSize(n->size);
            if (i.column() == Type) return QStringLiteral("文件夹");
        }
        if (role == Qt::ToolTipRole) return n->path;
        if (role == Qt::FontRole) { QFont font; font.setBold(true); return font; }
        return {};
    }
    const auto &g = scan.groups[n->group]; const auto &f = g.files[n->file];
    const bool chosen = g.keeper == n->file;
    const bool protectedA = scan.options.mode == Mode::Dual && f.side == "A";
    if (role == Qt::ToolTipRole) return f.path + QStringLiteral("\n重复组：%1\nSHA-256：\n").arg(n->group + 1) + QString::fromLatin1(f.sha256.toHex());
    if (role == Qt::CheckStateRole) {
        if (i.column() == Keep && (scan.options.mode == Mode::Single || f.side == "A")) return chosen ? Qt::Checked : Qt::Unchecked;
        if (i.column() == Replace && !chosen && !protectedA) return g.replace[n->file] ? Qt::Checked : Qt::Unchecked;
    }
    if (role == Qt::ForegroundRole) return QBrush(chosen || protectedA ? QColor("#13755d") : g.replace[n->file] ? QColor("#a65410") : QColor("#7b8494"));
    if (role == Qt::DisplayRole) {
        switch (i.column()) {
        case Name: return f.name;
        case Keep: return chosen ? (scan.options.mode == Mode::Dual ? QStringLiteral("链接目标") : QStringLiteral("原始文件")) : protectedA ? QStringLiteral("A 原件") : QString();
        case Replace: return !chosen && !protectedA ? (g.replace[n->file] ? QStringLiteral("→ .lnk") : QStringLiteral("跳过")) : QString();
        case Size: return readableSize(f.size);
        case Modified: return f.modified.toString("yyyy-MM-dd HH:mm:ss");
        case Type: return QFileInfo(f.name).suffix().isEmpty() ? QStringLiteral("无扩展名") : QFileInfo(f.name).suffix().toUpper();
        case Target: return g.replace[n->file] && g.keeper >= 0 ? QDir::toNativeSeparators(g.files[g.keeper].path) : QString();
        default: break;
        }
    }
    return {};
}
Qt::ItemFlags FileTreeModel::flags(const QModelIndex &i) const {
    if (!i.isValid()) return Qt::NoItemFlags;
    auto result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    const auto *n = node(i);
    if (!n->folder()) {
        result |= Qt::ItemNeverHasChildren;
        const auto &g = scan.groups[n->group]; const auto &f = g.files[n->file];
        if (i.column() == Keep && (scan.options.mode == Mode::Single || f.side == "A")) result |= Qt::ItemIsUserCheckable;
        if (i.column() == Replace && n->file != g.keeper && (scan.options.mode == Mode::Single || f.side == "B")) result |= Qt::ItemIsUserCheckable;
    }
    return result;
}
bool FileTreeModel::setData(const QModelIndex &i, const QVariant &value, int role) {
    if (!i.isValid() || role != Qt::CheckStateRole || !(flags(i) & Qt::ItemIsUserCheckable)) return false;
    const auto *n = node(i); auto &g = scan.groups[n->group];
    if (i.column() == Keep) {
        if (value.toInt() != Qt::Checked) return false; // Each group always has one target.
        setKeeper(g, scan.options.mode, n->file);
    } else if (i.column() == Replace) g.replace[n->file] = value.toInt() == Qt::Checked;
    else return false;
    notifyChanges({n->group}); return true;
}
void FileTreeModel::notifyGroup(int g) { for (auto *n : groupNodes[g]) emit dataChanged(forNode(n), forNode(n, ColumnCount - 1)); }
void FileTreeModel::updateTotals(int g) {
    selectedBytes -= groupSavings[g]; selectedCount -= groupCounts[g]; groupSavings[g] = 0; groupCounts[g] = 0;
    const auto &group = scan.groups[g];
    for (int f = 0; f < group.files.size(); ++f) if (group.replace[f]) { groupSavings[g] += group.files[f].size; ++groupCounts[g]; }
    selectedBytes += groupSavings[g]; selectedCount += groupCounts[g];
}
void FileTreeModel::notifyChanges(const QSet<int> &groups) {
    QVector<int> changed; changed.reserve(groups.size());
    for (int g : groups) { updateTotals(g); notifyGroup(g); changed.append(g); }
    emit groupsChanged(changed); emit planChanged();
}
void FileTreeModel::clear() { setResult({}); }
void FileTreeModel::setResult(ScanResult result) {
    beginResetModel(); scan = std::move(result); root = std::make_unique<Node>(); groupNodes.clear(); groupNodes.resize(scan.groups.size()); pathNodes.clear();
    selectedCount = 0; selectedBytes = 0; groupSavings.fill(0, scan.groups.size()); groupCounts.fill(0, scan.groups.size());
    auto folder = [this](Node *parent, const QString &name, const QString &path) {
        const auto key = path.toCaseFolded(); const auto found = pathNodes.constFind(key);
        if (found != pathNodes.constEnd()) return *found;
        auto n = std::make_unique<Node>(); n->name = name; n->path = path; n->parent = parent; n->row = int(parent->children.size());
        auto *out = n.get(); parent->children.push_back(std::move(n)); pathNodes.insert(key, out); return out;
    };
    auto addDirectories = [&](const QString &side, const QString &rootPath, const QStringList &directories) {
        if (rootPath.isEmpty()) return;
        auto *base = folder(root.get(), QStringLiteral("目录 %1  ·  ").arg(side) + QDir::toNativeSeparators(rootPath), rootPath);
        for (const auto &path : directories) {
            if (samePath(path, rootPath)) continue;
            QString cumulative = rootPath; auto *p = base;
            for (const auto &part : QDir(rootPath).relativeFilePath(path).split('/')) {
                cumulative = QDir(cumulative).filePath(part); p = folder(p, part, cumulative);
            }
        }
    };
    addDirectories("A", scan.options.rootA, scan.foldersA);
    if (scan.options.mode == Mode::Dual) addDirectories("B", scan.options.rootB, scan.foldersB);
    for (int g = 0; g < scan.groups.size(); ++g) for (int f = 0; f < scan.groups[g].files.size(); ++f) {
        const auto &file = scan.groups[g].files[f];
        Node *p = folder(root.get(), QStringLiteral("目录 %1  ·  ").arg(file.side) + QDir::toNativeSeparators(file.root), file.root);
        const QString relative = QDir(file.root).relativeFilePath(file.path);
        QString cumulative = file.root;
        const auto parts = relative.split('/');
        for (int j = 0; j + 1 < parts.size(); ++j) { cumulative = QDir(cumulative).filePath(parts[j]); p = folder(p, parts[j], cumulative); }
        auto n = std::make_unique<Node>(); n->name = file.name; n->path = file.path; n->parent = p; n->group = g; n->file = f;
        n->size = file.size; n->modified = file.modified; n->row = int(p->children.size());
        groupNodes[g].append(n.get()); pathNodes.insert(file.path.toCaseFolded(), n.get()); p->children.push_back(std::move(n));
        for (Node *ancestor = p; ancestor; ancestor = ancestor->parent) { ancestor->size += file.size; if (ancestor->modified < file.modified) ancestor->modified = file.modified; }
    }
    for (int g = 0; g < scan.groups.size(); ++g) updateTotals(g);
    sortNode(root.get()); endResetModel(); emit planChanged();
}
void FileTreeModel::sortNode(Node *p) {
    if (p->children.empty()) return;
    QCollator collator; collator.setNumericMode(true); collator.setCaseSensitivity(Qt::CaseSensitive);
    std::stable_sort(p->children.begin(), p->children.end(), [&](const auto &a, const auto &b) {
        if (a->folder() != b->folder()) return a->folder();
        int compare = 0;
        if (sortColumn == Size) compare = a->size < b->size ? -1 : a->size > b->size ? 1 : 0;
        else if (sortColumn == Modified) compare = a->modified < b->modified ? -1 : a->modified > b->modified ? 1 : 0;
        else if (sortColumn == Type) compare = collator.compare(QFileInfo(a->name).suffix(), QFileInfo(b->name).suffix());
        else if (sortColumn == Keep || sortColumn == Replace) {
            auto rank = [&](const Node *n) { if (n->folder()) return 0; const auto &g = scan.groups[n->group]; return sortColumn == Keep ? int(g.keeper == n->file) : int(g.replace[n->file]); };
            compare = rank(a.get()) - rank(b.get());
        }
        if (!compare) compare = collator.compare(a->name, b->name);
        if (!compare) compare = QString::compare(a->path, b->path);
        return sortOrder == Qt::AscendingOrder ? compare < 0 : compare > 0;
    });
    for (size_t i = 0; i < p->children.size(); ++i) { p->children[i]->row = int(i); sortNode(p->children[i].get()); }
}
void FileTreeModel::sort(int column, Qt::SortOrder order) {
    if (column < 0 || column >= ColumnCount) return;
    sortColumn = column; sortOrder = order;
    const auto persistent = persistentIndexList();
    emit layoutAboutToBeChanged(); sortNode(root.get());
    QModelIndexList updated; for (const auto &i : persistent) updated.append(forNode(node(i), i.column()));
    changePersistentIndexList(persistent, updated); emit layoutChanged();
}
QVector<Operation> FileTreeModel::plan() const {
    QVector<Operation> result;
    for (const auto &g : scan.groups) if (g.keeper >= 0) for (int i = 0; i < g.files.size(); ++i) if (g.replace[i]) result.append({g.files[i], g.files[g.keeper]});
    return result;
}
quint64 FileTreeModel::saving() const { return selectedBytes; }
QModelIndex FileTreeModel::indexForFile(int group, int file) const {
    if (group < 0 || group >= groupNodes.size()) return {};
    return file < 0 || file >= groupNodes[group].size() ? QModelIndex() : forNode(groupNodes[group][file]);
}
QModelIndex FileTreeModel::indexForPath(const QString &path) const {
    return forNode(pathNodes.value(cleanPath(path).toCaseFolded(), nullptr));
}
void FileTreeModel::collect(Node *n, QSet<int> &groups) const { if (!n->folder()) groups.insert(n->group); for (const auto &c : n->children) collect(c.get(), groups); }
QSet<int> FileTreeModel::selectedGroups(const QModelIndexList &selected) const {
    QSet<int> groups;
    if (selected.isEmpty()) collect(root.get(), groups);
    else for (const auto &i : selected) collect(node(i), groups);
    return groups;
}
QString FileTreeModel::chooseSelected(const QModelIndexList &selected) {
    QMap<int, int> choices;
    for (const auto &i : selected) {
        auto *n = node(i); if (n->folder()) continue;
        if (scan.options.mode == Mode::Dual && scan.groups[n->group].files[n->file].side != "A") return QStringLiteral("双目录模式的保留目标只能从 A 目录选择。");
        if (choices.contains(n->group) && choices[n->group] != n->file) return QStringLiteral("同一重复组选择了多份文件，请每组仅选一份保留目标。");
        choices[n->group] = n->file;
    }
    if (choices.isEmpty()) return QStringLiteral("请选中一个或多个文件行；每个重复组选择一份。");
    QSet<int> touched;
    for (auto it = choices.constBegin(); it != choices.constEnd(); ++it) { setKeeper(scan.groups[it.key()], scan.options.mode, it.value()); touched.insert(it.key()); }
    notifyChanges(touched); return {};
}
void FileTreeModel::applyRule(const QModelIndexList &selected, KeepRule rule) {
    const auto groups = selectedGroups(selected);
    for (int g : groups) setKeeper(scan.groups[g], scan.options.mode, pickKeeper(scan.groups[g], scan.options.mode, rule));
    notifyChanges(groups);
}
void FileTreeModel::preferFolders(const QModelIndexList &selected) {
    QStringList folders;
    for (const auto &i : selected) if (node(i)->folder()) folders.append(node(i)->path);
    QSet<int> touched;
    for (int g : selectedGroups(selected)) {
        DuplicateGroup candidates = scan.groups[g];
        QVector<int> mapping; candidates.files.clear();
        for (int f = 0; f < scan.groups[g].files.size(); ++f) {
            const auto &file = scan.groups[g].files[f];
            bool match = false; for (const auto &path : folders) match |= withinRoot(file.path, path);
            if (match && (scan.options.mode == Mode::Single || file.side == "A")) { candidates.files.append(file); mapping.append(f); }
        }
        const int best = pickKeeper(candidates, scan.options.mode, KeepRule::Shallowest);
        if (best >= 0) { setKeeper(scan.groups[g], scan.options.mode, mapping[best]); touched.insert(g); }
    }
    notifyChanges(touched);
}
void FileTreeModel::setReplacement(const QModelIndexList &selected, bool enabled) {
    QSet<int> touched;
    std::function<void(Node *)> visit = [&](Node *n) {
        if (!n->folder()) {
            auto &g = scan.groups[n->group]; const auto &file = g.files[n->file];
            if (n->file != g.keeper && (scan.options.mode == Mode::Single || file.side == "B")) { g.replace[n->file] = enabled; touched.insert(n->group); }
        }
        for (const auto &c : n->children) visit(c.get());
    };
    for (const auto &i : selected) visit(node(i));
    notifyChanges(touched);
}
void FileTreeModel::resetDefaults() {
    QSet<int> touched;
    for (int g = 0; g < scan.groups.size(); ++g) {
        scan.groups[g].replace.clear();
        setKeeper(scan.groups[g], scan.options.mode, pickKeeper(scan.groups[g], scan.options.mode, KeepRule::Shallowest)); touched.insert(g);
    }
    notifyChanges(touched);
}
}
