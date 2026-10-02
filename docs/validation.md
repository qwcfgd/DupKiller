# 验证记录

## 2026-10-02：双栏界面与发布前验证

Qt 5.15.19 / GCC 8.1.0 和 Qt 6.8.4 / GCC 14.2.0 的 Release 构建、依赖部署及 CTest 均通过；两套 QtTest 各为 **29 passed / 0 failed / 0 skipped**。这次修改集中在界面和模型投影，未改动扫描、替换及恢复引擎。

新增检查：每个副本与选定原件同排，单目录保留目标变更后正确重建配对，双目录目标只允许 A，排序后持久索引仍对应同一文件；勾选副本不重置模型且保留选择和滚动位置。70 个副本的窗口测试验证单/双目录输入布局、左右共享选择、纵向同步滚动、文件夹同步折叠和展开、可调整分隔条，以及列表滚动后固定根行仍完整显示。

两套已部署 EXE 在仅系统 PATH 下分别运行单目录、双目录和滚动到底部的只读 smoke 模式，六次均退出码 0。已检查最新截图；公开截图使用自动生成的演示文件（A 39 个文件，B 42 个副本），并保存于 `docs/screenshots/`。单目录截图使用另一个小型演示目录。原始用户样本未用于公开截图。

构建目录 `../build/QtDupKiller-qt5` 和 `../build/QtDupKiller-qt6` 中保存本轮 `test-results.txt` / `test-results.xml`。本轮界面截图位于 `test-output/qt5-paired-*.png`、`test-output/qt6-paired-*.png`。此前完整样本验证如下，原始样本及 A/B 副本均不发布到 GitHub。

## 2026-10-01：替换引擎、样本与图标

原始 `testsrc` 始终只读；删除和回收仅针对当日创建的 A/B 副本及自动化临时样例，没有运行样本中的 EXE。

| 项目 | Qt 5 | Qt 6 |
|---|---|---|
| Qt 版本 | 5.15.19 | 6.8.4 |
| GCC 实际版本 | 8.1.0 | 14.2.0 |
| CMake Release 构建 | 成功 | 成功 |
| QtTest | 26 passed / 0 failed / 0 skipped | 26 passed / 0 failed / 0 skipped |
| CTest | 1/1 passed | 1/1 passed |
| 已部署目录再次构建并测试 | 通过 | 通过 |
| 发布目录运行（仅系统 PATH） | 成功，退出码 0 | 成功，退出码 0 |
| Windows 原生平台界面截图 | 已查看 | 已查看 |
| 底部进度与任务按钮同排 | 通过 | 通过 |
| 忙碌标题提示及结束恢复 | 通过 | 通过 |
| Qt 标题栏 ICO 资源 | 通过 | 通过 |
| EXE 内嵌图标提取 | 通过 | 通过 |

构建及部署目录为 `../build/QtDupKiller-qt5` 和 `../build/QtDupKiller-qt6`。26 项 QtTest 包括初始化/清理和参数化样例，详细结果位于各目录的 `test-results.txt`、`test-results.xml`。

主要覆盖：三条件严格匹配、两种模式、A 全部保留、B 独有重复不处理、名称大小写、系统/链接/硬链接排除、占用与取消、USN 安全解析、最浅/最早/最新保留规则、多选、目录层级排序、持久模型索引、SHA-256 仅 tooltip、默认不永久删除、真实永久替换、同大小同时间内容变更拒绝、快捷方式冲突、回收站替换及 Shell 恢复、事务中断恢复、父目录锁定、异步 ViewModel 和清单失效。本次新增命名数据流相同/不同/扫描后变化校验，普通/空/下载安全数据流完整复制，以及携带数据流文件的永久删除与回收恢复。

回收站单元测试在沙箱外以普通桌面用户权限运行，仅操作构建目录临时样例；成功回收的单元样例随后恢复并清理。offscreen 字体目录有一条 Qt 5 字体解析调试信息，实际 Windows 平台截图中文显示正常。

部署后的 Qt DLL 会使用应用目录作为插件前缀，CMake 已固定 CTest 的 `QT_QPA_PLATFORM_PLUGIN_PATH` 为所选 Qt Kit 的平台插件目录；重新运行完整构建脚本后，两套 CTest 均通过。

## testsrc 完整 A/B 验证

源文件：96 个，8,608,686,570 字节，约 8.02 GiB，包含多层目录、中文名称、照片、文本、EXE、MSI 和压缩包。10 个文件具有 `Zone.Identifier` 或 `SmartScreen` 命名数据流。

旧版复现：通过 CopyFileW 复制 A/B，保留原属性、时间和数据流。最终仅 86 个 B 文件替换成功，剩余 10 个均因旧版排除备用数据流而未入清单。原始 testsrc 和 A 均未变。报告为 `test-output/ab-before/report.json`。

修复后两套程序重新从 testsrc 建立独立副本：

| 核验项 | Qt 5 / 原生遍历 / 永久删除 | Qt 6 / 自动后端 / 回收站 |
|---|---|---|
| 样本目录 | `test-output/ab-qt5` | `test-output/ab-qt6` |
| 复制的 A/B 文件 | 各 96 个 | 各 96 个 |
| 清单 / 成功 / 失败 | 96 / 96 / 0 | 96 / 96 / 0 |
| B 最终文件 | 96 个 `.lnk` | 96 个 `.lnk` |
| B 剩余原始文件 | 0 | 0 |
| 原始 testsrc 变化 | 0 | 0 |
| A 原件变化 | 0 | 0 |
| 快捷方式数据流丢失 | 0 | 0 |
| 普通桌面 Shell 目标核验错误 | 0 / 96 | 0 / 96 |
| 验证工具退出码 | 0 | 0 |

原始 testsrc 和 A 的核验逐文件比较完整 SHA-256、每个命名流 SHA-256、修改时间和属性。B 的每个链接验证目标存在于 A，并比较原始副本与链接的命名流。另用普通桌面 WScript.Shell 独立核验 192 个链接，全部指向对应 A 原件，未打开目标文件。Qt 6 自动后端因普通用户无法读取卷而回退原生遍历。

结果清单：`test-output/ab-qt5/report.json`、`test-output/ab-qt6/report.json`。初始指纹：对应目录的 `baseline.json`；事务日志：对应目录的 `logs/`。Qt 5 的 B 副本已永久删除；Qt 6 的 B 副本在回收站中，可用该轮日志恢复。最终 A/B 副本保留供用户检查。

## 界面与图标

截图：`test-output/qt5-preview.png`、`test-output/qt6-preview.png`。Smoke 模式只读扫描 `test-output/preview-fixture`，不执行替换；窗口隐藏导出截图。进度条位于最底行，与“开始任务：执行清单”按钮平行。

根目录 `QtDupKiller.ico` 为原创文档/快捷方式箭头图标，包含 16/24/32/48/64/128/256 像素。QtTest 验证窗口图标资源非空；通过 Windows ExtractAssociatedIcon 提取两套 EXE 的图标，输出 `test-output/qt5-exe-icon.png`、`test-output/qt6-exe-icon.png` 并进行视觉检查。

MFT/USN：已验证解析、异常回退及普通权限后备扫描；当前运行环境未验证管理员权限下的完整卷枚举与性能。管理员权限并非原生后备扫描的前提。

部署目录包含 Qt 和匹配 GCC 运行库。Qt 6 的 windeployqt 对未安装的 DXC 组件有提示；本项目 Widgets 在仅系统 PATH 下启动及只读扫描均成功。
