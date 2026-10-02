#pragma once
#include "domain.h"
namespace dup {
class Executor {
public:
    static ExecutionResult execute(const QVector<Operation> &plan, ScanOptions options, bool permanent,
                                   const QString &logDirectory, const Cancel &cancel, const Progress &progress = {});
    static ExecutionResult recover(const QString &journal, const Cancel &cancel, const Progress &progress = {});
};
}
