#pragma once
#include "domain.h"
#include <QHash>

namespace dup {
struct MftEntry { quint64 id = 0, parent = 0; QString name; quint32 attributes = 0; };
// Shared parser for enumeration and journal reconciliation; rejects unsupported layouts.
bool parseUsnBuffer(const QByteArray &buffer, QHash<quint64, MftEntry> &entries,
                    bool changes, qint64 upperUsn, QString &error);
class Scanner {
public:
    static ScanResult scan(ScanOptions options, const Cancel &cancel, const Progress &progress = {});
};
}
