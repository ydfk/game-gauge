# 游戏仪表 · GameGauge

Windows 11 游戏内性能监控的原生 C++ 桌面程序。当前处于技术验证阶段，工作名称与界面可继续调整；完整验收状态见 [plan.md](plan.md)。

## 已有能力

- 托盘菜单、全局快捷键、独立设置窗口和顶部高密度监控条。
- 自动发现 CPU、GPU、显示器、DPI/HDR；读取 Windows 计数器与可用的 NVIDIA 遥测。
- PresentMon SDK 帧采集接口、帧时间与低帧统计、会话 JSON 导出。
- PresentMon 指标能力诊断；游戏失焦或手动暂停时暂停有效会话统计。
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

编辑位置时拖动监控条，按 Enter 保存或 Esc 取消；也可从托盘菜单保存。生成便携包运行 `./scripts/package-portable.ps1`，产物位于 `build/package/`。该包是技术预览，已在一款真实游戏与 OBS 游戏采集中完成初步验证，其他采集方法和长期稳定性尚未通过。

`./scripts/smoke-host.ps1` 会在隔离的 `build/runtime-test/smoke` 目录验证宿主启动、连续 IPC、配置提交和持久化。运行前请退出正在使用的游戏仪表，脚本会拒绝触碰已有进程。

FPS 依赖 Intel PresentMon Shared Service。当前工程不会在启动时静默安装系统服务。
`GameGauge.Diagnostics.exe --presentmon-capabilities` 可列出当前服务逐设备报告的指标能力；
`GameGauge.Diagnostics.exe --presentmon-probe <游戏 PID> --sample-ms 10000` 可检查原始帧与有效帧数量。
游戏 PID 可先通过 `GameGauge.Diagnostics.exe --targets` 查找。

源码构建额外生成 `GameGauge.CpuProbe.exe`，用于验证原生 CPU 温度路径。研究用 PawnIO 模块不在仓库和便携包内；从 [PawnIO.Modules 官方 0.2.11 发行版](https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.11)取得 `AMDFamily17.bin` 后放到 `.deps/AMDFamily17-0.2.11.bin`。探针内置官方模块 SHA-256 校验，只读取 SMN 温度寄存器。在管理员 PowerShell 中运行：

```powershell
./build/core/bin/Release/GameGauge.CpuProbe.exe ./.deps/AMDFamily17-0.2.11.bin
```

本机一次读数为 Ryzen 9 9950X3D `Tctl=65.75°C`；普通权限连接 PawnIO 被拒绝。该探针不代表常驻采样或其他 CPU 型号已经完成，详见 [实机验收记录](docs/validation.md)。

## 当前限制

- CPU 真实温度已经由独立原生探针在本机读到一次 Tctl，但尚未接入常驻宿主。本机 PresentMon 2.6.0 的能力目录将 `cpu_temperature` 标为 `not_implemented_by_presentmon`；宿主仍显示“—”，不会把 ACPI 热区温度冒充 CPU 温度。
- 当前机器的 PresentMon 服务已在《控制：共振》取得真实有效帧；内置隐藏 DX11 测试窗口没有可读帧。1%/0.1% Low 的游戏内精度、跨游戏兼容性仍待验收。
- 用户已确认无边框与全屏模式下玩家屏幕可见 HUD，OBS 游戏采集录屏中无 HUD；本项目尚未独立检查录制文件画面。OBS 窗口/显示器采集、受保护游戏与反作弊环境仍待验收。
- 设置页已可鼠标与基本键盘操作，读屏支持、较小屏幕适配、每游戏配置、安装包和长期稳定性测试仍待完成。

第三方来源和许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
游戏、双屏 HDR 与 OBS 的逐项检查见 [实机验收记录](docs/validation.md)。
