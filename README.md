# 游戏仪表 · GameGauge

Windows 11 游戏内性能监控的原生 C++ 桌面程序。当前处于技术验证阶段，工作名称与界面可继续调整；完整验收状态见 [plan.md](plan.md)。

## 已有能力

- 托盘菜单、全局快捷键、独立设置窗口和顶部高密度监控条。
- 自动发现 CPU、GPU、显示器、DPI/HDR；读取 Windows 计数器与可用的 NVIDIA 遥测。
- PresentMon SDK 帧采集接口、帧时间与低帧统计、会话 JSON 导出。
- 请求 Windows 排除窗口捕获，并把“系统接受请求”和“OBS 实际验证”分开显示。
- 监控内容、顺序、位置、字号、透明度、设备与游戏目标设置。

## 便携包运行

解压 `GameGauge-0.1.0-win-x64.zip` 后，双击 `GameGauge.exe`。程序驻留在系统托盘；左键托盘图标打开设置，右键打开菜单。首次运行会在当前用户目录保存配置。默认自动寻找前台无边框大窗口；普通窗口化游戏可在“设备与游戏”中手动选定。没有帧采集服务时，FPS 显示为不可用，仍可预览界面和查看其他可用指标。

便携包不包含 PresentMon 服务、驱动或 MSI；它是已安装服务的客户端，不是完整免安装采集套装。如需真实 FPS，请从 [Intel 官方 PresentMon 发行页](https://github.com/GameTechDev/PresentMon/releases)下载正式 MSI，在**管理员 PowerShell** 中切换到解压目录后运行：

```powershell
./scripts/install-presentmon.ps1 -MsiPath 'C:\路径\PresentMon-2.6.0.msi' -VerifyOnly
./scripts/install-presentmon.ps1 -MsiPath 'C:\路径\PresentMon-2.6.0.msi'
```

脚本会核验 Intel 数字签名和产品名，已有服务时不会覆盖。安装后重启游戏仪表。在游戏运行时执行 `./GameGauge.Diagnostics.exe --host-status`，检查 `snapshot.fps.state` 是否为 `valid`。安装和运行诊断如有失败，请保留完整错误信息；不要反复安装。

## 从源码构建

在 Windows 11 x64、Visual Studio 2022 C++ 桌面工具和 Windows SDK 环境中运行：

```powershell
./scripts/build-core.ps1
./build/core/bin/Release/GameGauge.exe
```

启动后打开设置窗口，关闭设置窗口时宿主程序留在托盘；再次双击主程序会重新打开设置窗口。快捷键为 `Ctrl+Alt+Shift+F6` 显示/隐藏、`F7` 编辑位置、`F8` 重置统计。`GameGauge.Diagnostics.exe --host-status` 输出采集与覆盖层诊断；`--targets` 列出当前可发现的窗口。

编辑位置时拖动监控条，按 Enter 保存或 Esc 取消；也可从托盘菜单保存。生成便携包运行 `./scripts/package-portable.ps1`，产物位于 `build/package/`。该包是技术预览，尚未通过真实游戏、OBS 录制和长期稳定性验收。

`./scripts/smoke-host.ps1` 会在隔离的 `build/runtime-test/smoke` 目录验证宿主启动、连续 IPC、配置提交和持久化。运行前请退出正在使用的游戏仪表，脚本会拒绝触碰已有进程。

FPS 依赖 Intel PresentMon Shared Service。当前工程不会在启动时静默安装系统服务。

## 当前限制

- CPU 真实温度尚未通过实机验证；当前采集链在服务提供且声明可用时才尝试读取，缺失时显示“—”，不会把 ACPI 热区温度冒充 CPU 温度。
- 当前机器的 PresentMon 服务已运行，但内置 DX11 测试窗口尚未产生可读帧；真实游戏 FPS、1%/0.1% Low 不能视为已验收。
- Windows 已接受捕获排除请求，但 OBS 窗口采集的录制文件尚未核验。独占全屏、受保护游戏与反作弊环境不在当前验收范围。
- 设置页已可鼠标与基本键盘操作，读屏支持、较小屏幕适配、每游戏配置、安装包和长期稳定性测试仍待完成。

第三方来源和许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
游戏、双屏 HDR 与 OBS 的逐项检查见 [实机验收记录](docs/validation.md)。
