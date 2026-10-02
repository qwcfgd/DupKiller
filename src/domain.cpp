#include "domain.h"
#include "windowsfs.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
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
bool photoName(const QString &name) {
    static const QSet<QString> extensions{"jpg", "jpeg", "jpe", "png", "heic", "heif", "tif", "tiff", "bmp", "gif", "webp", "avif", "dng", "cr2", "cr3", "nef", "nrw", "arw", "orf", "rw2", "raf", "pef", "srw"};
    return extensions.contains(QFileInfo(name).suffix().toLower());
}
bool datedDescriptionFolder(const FileRecord &file) {
    // Support the album's year-only, year/month and full-date descriptions.
    static const QRegularExpression pattern(QStringLiteral("^((?:19|20)\\d{2})(?:[.\\-年](\\d{1,2})(?:[.\\-月](\\d{1,2}))?)?[.年日\\s_-]*([^\\d.\\s_-].*)$"));
    const auto parts = QDir(file.root).relativeFilePath(QFileInfo(file.path).absolutePath()).split('/');
    for (const auto &part : parts) {
        const auto match = pattern.match(part);
        if (!match.hasMatch()) continue;
        const int month = match.captured(2).toInt(), day = match.captured(3).toInt();
        if (!match.captured(2).isEmpty() && (month < 1 || month > 12)) continue;
        if (!match.captured(3).isEmpty() && !QDate(match.captured(1).toInt(), month, day).isValid()) continue;
        return true;
    }
    return false;
}
int pickKeeper(const DuplicateGroup &g, Mode mode, KeepRule rule) {
    int best = -1;
    bool bestDated = false;
    for (int i = 0; i < g.files.size(); ++i) {
        const auto &f = g.files[i];
        if (mode == Mode::Dual && f.side != "A") continue;
        const bool dated = rule == KeepRule::PreferNonDated && datedDescriptionFolder(f);
        if (best < 0) { best = i; bestDated = dated; continue; }
        const auto &b = g.files[best];
        bool better = false, equal = false;
        if (rule == KeepRule::PreferNonDated && dated != bestDated) better = !dated;
        else if (rule == KeepRule::Shallowest || rule == KeepRule::PreferNonDated) { better = f.depth < b.depth; equal = f.depth == b.depth; }
        else { better = rule == KeepRule::Oldest ? f.writeTicks < b.writeTicks : f.writeTicks > b.writeTicks; equal = f.writeTicks == b.writeTicks; }
        if (better || (equal && QString::compare(f.path, b.path, Qt::CaseSensitive) < 0)) { best = i; bestDated = dated; }
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
