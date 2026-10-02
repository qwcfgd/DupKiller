#include "domain.h"
#include "windowsfs.h"
#include <QDir>
#include <QFileInfo>
#include <algorithm>

namespace dup {
QString cleanPath(const QString &p) { return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(p).absoluteFilePath())); }
bool samePath(const QString &a, const QString &b) { return cleanPath(a).compare(cleanPath(b), Qt::CaseInsensitive) == 0; }
bool withinRoot(const QString &p, const QString &r) {
    const QString root = cleanPath(r), path = cleanPath(p);
    return path.compare(root, Qt::CaseInsensitive) == 0 || path.startsWith(root.endsWith('/') ? root : root + '/', Qt::CaseInsensitive);
}
QString validateRoots(ScanOptions &o) {
    if (o.rootA.trimmed().isEmpty()) return QStringLiteral("请选择目录 A。");
    o.rootA = cleanPath(o.rootA);
    QString e;
    if (!win::safeDirectory(o.rootA, e)) return QStringLiteral("目录 A：") + e;
    if (o.mode == Mode::Dual) {
        if (o.rootB.trimmed().isEmpty()) return QStringLiteral("请选择目录 B。");
        o.rootB = cleanPath(o.rootB);
        if (!win::safeDirectory(o.rootB, e)) return QStringLiteral("目录 B：") + e;
        if (withinRoot(o.rootA, o.rootB) || withinRoot(o.rootB, o.rootA)) return QStringLiteral("A 与 B 不能相同或互相包含。");
    }
    return {};
}
int pickKeeper(const DuplicateGroup &g, Mode mode, KeepRule rule) {
    int best = -1;
    for (int i = 0; i < g.files.size(); ++i) {
        const auto &f = g.files[i];
        if (mode == Mode::Dual && f.side != "A") continue;
        if (best < 0) { best = i; continue; }
        const auto &b = g.files[best];
        bool better = false, equal = false;
        if (rule == KeepRule::Shallowest) { better = f.depth < b.depth; equal = f.depth == b.depth; }
        else { better = rule == KeepRule::Oldest ? f.writeTicks < b.writeTicks : f.writeTicks > b.writeTicks; equal = f.writeTicks == b.writeTicks; }
        if (better || (equal && QString::compare(f.path, b.path, Qt::CaseSensitive) < 0)) best = i;
    }
    return best;
}
void setKeeper(DuplicateGroup &g, Mode mode, int index) {
    if (index < 0 || index >= g.files.size() || (mode == Mode::Dual && g.files[index].side != "A")) return;
    const int previous = g.keeper;
    const bool initialized = g.replace.size() == g.files.size();
    g.keeper = index;
    g.replace.resize(g.files.size());
    for (int i = 0; i < g.files.size(); ++i) {
        if (!initialized || (i == previous && previous != index)) g.replace[i] = i != index && (mode == Mode::Single || g.files[i].side == "B");
        if (i == index || (mode == Mode::Dual && g.files[i].side == "A")) g.replace[i] = false;
    }
}
QString readableSize(quint64 n) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = double(n); int unit = 0;
    while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
    return QString::number(value, 'f', unit == 0 ? 0 : 2) + ' ' + units[unit];
}
}
