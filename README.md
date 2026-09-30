# 游戏仪表 · GameGauge

Windows 11 游戏内性能监控的原生 C++ 桌面程序。当前处于技术验证阶段，工作名称与界面可继续调整；完整验收状态见 [plan.md](plan.md)。

## 已有能力

- 托盘菜单、全局快捷键、独立设置窗口和顶部高密度监控条。
- 自动发现 CPU、GPU、显示器、DPI/HDR；读取 Windows 计数器与可用的 NVIDIA 遥测。
- PresentMon SDK 帧采集接口、帧时间与低帧统计、会话 JSON 导出。
- PresentMon 指标能力诊断；游戏失焦或手动暂停时暂停有效会话统计。
- 请求 Windows 排除窗口捕获，并把“系统接受请求”和“OBS 实际验证”分开显示。
- 监控内容、顺序、位置、字号、透明度、设备与游戏目标设置。

## 安装与使用

运行 `GameGauge-0.1.9-Setup.exe`，同意 Windows 管理员提示即可。安装器内含经过哈希及签名校验的官方 PresentMon / PawnIO 安装程序和 AMD 温度模块，自动注册温度服务并启动普通权限主程序。桌面、开始菜单和已安装应用显示“游戏仪表”。开始菜单快捷方式保留英文文件名 `GameGauge.lnk`，通过 Shell 本地化名称显示中文，为英文搜索保留索引名称；也可使用“卸载游戏仪表”。运行库静态链接，无需额外安装 Visual C++ 运行库。已有采集依赖会保留。

安装窗口与设置采用统一的深蓝与薄荷绿风格，显示实际安装阶段和进度；完成后自动启动，失败时保留阶段并提供“打开日志”。安装进行中不能关闭，完成后可通过按钮、标题关闭或键盘退出。

OBS 状态标记默认关闭，在“监控项目”选择“OBS 录制状态”后启用；已有明确保存的开关偏好会保留。关闭时不主动连接 OBS。启用前在 OBS“工具 → WebSocket 服务器设置”启用服务器；程序会读取当前用户标准 OBS 安装的本机连接配置，支持密码验证，不保存密码。标记区分红色“录制中”、黄色“已暂停”、“未录制”与“未连接”；不会控制 OBS 录制。OBS 未启动或连接失败不会被误报为停止录制。便携 OBS 的自定义配置目录暂不支持。

默认自动识别常见游戏目录、Unity/Unreal/Steam 引擎和无边框游戏，收到游戏帧后显示监控。普通窗口游戏也可识别；漏识别时在“游戏与排除”选择程序并记住。排除名单按进程文件名保存。切到桌面、最小化或游戏退出时监控条隐藏。

设置仅保留外观、按 CPU/GPU 分组的监控项目、游戏与排除、游戏历史。位置按钮直接生效于游戏客户区；顶部默认零边距。每次游戏记录保存在 `%LOCALAPPDATA%\GameGauge\history`，每五秒写入，退出后保留。平均 FPS 是前台采样时间加权均值。游戏菜单继续呈现时显示实时 FPS；已确认游戏停止呈现时短暂保留后显示 0 FPS，而不是缺失值。

温度服务当前支持 AMD Zen（family 0x17–0x1a），不把其他传感器读数冒充 CPU 温度。服务只发布温度，普通用户仅有读取权限。卸载保留用户配置、历史及共享的第三方依赖。

“监控项目”新增硬盘温度，HUD 显示有有效读数的最热硬盘；点击“查看各硬盘温度”可逐块查看设备名称、温度和不可用原因。每五秒通过 Windows 存储温度接口查询；本机三块 NVMe 已在普通用户权限下验证，部分 SATA/USB/RAID 驱动不提供此接口，不可用温度不会显示为零。

游戏历史支持按日期筛选、游玩时长汇总、单局平均/最高/最低指标，以及性能报告、性能图表、会话事件和设备信息。记录 CPU/GPU 占用和温度、FPS、帧时间、内存、显存、GPU 功耗及最热硬盘温度；支持导出完整 JSON。曲线初始每秒采样，长会话逐级降低分辨率，统计仍累计全部有效前台采样。失焦、暂停和无效数据留缺口。Low 显示最后一次最近 60 秒窗口读数，不冒充整局 Low。旧记录保留原有汇总，不补造历史曲线、平均温度或设备信息。

顶部游玩时长与历史使用同一累计值；切回仍在运行的游戏时恢复原有时长，显示模式变化或重建帧采集不清零。记录过滤零 FPS、非有限值和超出指标范围的值，保留真实低 FPS 与正常的零占用；切回游戏后跨越后台时间的帧间隔不计入统计。被过滤的曲线采样显示缺口。旧记录中的无效极值在查看和导出时隐藏，原文件和有效平均值保留，不从降采样曲线重算。

## 开发用便携包

解压 `GameGauge-0.1.0-win-x64.zip` 后，双击 `GameGauge.exe`。程序驻留在系统托盘；左键托盘图标打开设置，右键打开菜单。首次运行会在当前用户目录保存配置。默认自动寻找前台无边框大窗口；普通窗口化游戏可在“设备与游戏”中手动选定。没有帧采集服务时，FPS 显示为不可用，仍可预览界面和查看其他可用指标。

便携包不包含 PresentMon 服务、驱动或 MSI；它是已安装服务的客户端，不是完整免安装采集套装。如需真实 FPS，请从 [Intel 官方 PresentMon 发行页](https://github.com/GameTechDev/PresentMon/releases)下载正式 MSI，在**管理员 PowerShell** 中切换到解压目录后运行：

```powershell
./scripts/install-presentmon.ps1 -MsiPath 'C:\路径\PresentMon-2.6.0.msi' -VerifyOnly
./scripts/install-presentmon.ps1 -MsiPath 'C:\路径\PresentMon-2.6.0.msi'
```

脚本会核验 Intel 数字签名和产品名，已有服务时不会覆盖。安装后重启游戏仪表。在游戏运行时执行 `./GameGauge.Diagnostics.exe --host-status`，检查 `snapshot.fps.state` 是否为 `valid`。安装和运行诊断如有失败，请保留完整错误信息；不要反复安装。

## 从源码构建

Tag 自动发布和软件更新使用方法见 [发布与更新](docs/releasing.md)。设置中的“版本与更新”支持手动更新、自动检查，以及可选的游戏退出后自动下载安装。GitHub 构建自动注入仓库地址和 Tag 版本。

在 Windows 11 x64、Visual Studio 2022 C++ 桌面工具和 Windows SDK 环境中运行：

```powershell
.\build.cmd                  # 编译 Release 并运行核心测试
.\build.cmd -Task Run        # 编译后打开程序
.\build.cmd -Task Package    # 自动下载校验依赖并生成完整安装包
```

也可在 PowerShell 使用 `./build.ps1`，参数相同。可选 `-Configuration Debug`（仅 Build/Run）、`-Version 0.1.2`、`-Repository owner/repo`。安装包位于 `build/package/GameGauge-版本-Setup.exe`，可执行文件位于 `build/core/bin/Release`。构建需要 VS 2022 的 C++ 桌面工具、Windows SDK 和 CMake 组件；首次打包需要网络，普通编译不下载驱动依赖。

启动后打开设置窗口，标题区仅保留最大化／还原和关闭；支持拖动标题区及调整窗口大小。关闭设置时宿主程序留在托盘，再次打开保留已有窗口的最大化状态。设置侧栏的“退出程序”和托盘“退出”都会关闭设置及监控条；主程序意外终止后设置也会退出，单独启动设置程序会启动完整应用。快捷键为 `Ctrl+Alt+Shift+F6` 显示/隐藏、`F7` 编辑位置、`F8` 重置统计。`GameGauge.Diagnostics.exe --host-status` 输出采集与覆盖层诊断；`--targets` 列出当前可发现的窗口。

编辑位置时拖动监控条，按 Enter 保存或 Esc 取消；也可从托盘菜单保存。生成便携包运行 `./scripts/package-portable.ps1`，产物位于 `build/package/`。该包是技术预览，已在一款真实游戏与 OBS 游戏采集中完成初步验证，其他采集方法和长期稳定性尚未通过。

`./scripts/smoke-host.ps1` 会在隔离的 `build/runtime-test/smoke` 目录验证宿主启动、连续 IPC、配置提交和持久化。运行前请退出正在使用的游戏仪表，脚本会拒绝触碰已有进程。

`./scripts/test-settings-window.ps1` 验证设置窗口的最小化禁用、标题与边缘命中、最大化／还原、任务栏边界、托盘重复打开、关闭后重开及“退出程序”。脚本仅启动隔离测试实例，会短暂显示测试窗口；已有宿主或设置进程时拒绝执行。

`./scripts/test-setup-ui.ps1` 用独立的普通权限 `GameGauge.SetupPreview.exe` 验证安装界面的三种状态、三档 DPI、关闭保护和按钮／键盘交互。预览程序不包含安装执行入口和安装资源，不修改服务或已安装应用；它不进入安装包。直接运行该程序可查看界面，使用 `--state success` 或 `--state failure` 查看终态。

FPS 依赖 Intel PresentMon Shared Service。当前工程不会在启动时静默安装系统服务。
`GameGauge.Diagnostics.exe --presentmon-capabilities` 可列出当前服务逐设备报告的指标能力；
`GameGauge.Diagnostics.exe --presentmon-probe <游戏 PID> --sample-ms 10000` 可检查原始帧与有效帧数量。
游戏 PID 可先通过 `GameGauge.Diagnostics.exe --targets` 查找。

开发用便携包保留 `GameGauge.CpuProbe.exe` 单次诊断入口；正式安装版自动运行 `GameGauge.Sensor` 服务，不需要手动启动探针。源码构建的探针要求 `.deps/AMDFamily17-0.2.11.bin`，并校验官方模块 SHA-256。单次诊断命令：

```powershell
./build/core/bin/Release/GameGauge.CpuProbe.exe ./.deps/AMDFamily17-0.2.11.bin
```

本机一次性探针读数为 Ryzen 9 9950X3D `Tctl=65.75°C`；普通权限连接 PawnIO 被拒绝。独立管理员进程启动后，普通权限主程序已连续读到约 `66.875 → 67°C` 的 Tctl。其他 CPU 型号和长时间稳定性尚未验证，详见 [实机验收记录](docs/validation.md)。

## 当前限制

- CPU 温度已接入安装器管理的服务；原有独立进程在本机实测有效。新服务的实机安装验收、长时间稳定性、温度对照与跨型号支持仍待验证。
- 当前机器的 PresentMon 服务已在《控制：共振》取得真实有效帧；内置隐藏 DX11 测试窗口没有可读帧。1%/0.1% Low 的游戏内精度、跨游戏兼容性仍待验收。
- 用户已确认无边框与全屏模式下玩家屏幕可见 HUD，OBS 游戏采集录屏中无 HUD；本项目尚未独立检查录制文件画面。OBS 窗口/显示器采集、受保护游戏与反作弊环境仍待验收。
- 新设置页已经渲染验证；读屏支持、跨显示器 DPI 切换、每游戏独立外观配置及长期稳定性仍待验证。自动识别采用启发式，不能保证覆盖所有游戏。

第三方来源和许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
游戏、双屏 HDR 与 OBS 的逐项检查见 [实机验收记录](docs/validation.md)。

## 0.1.4 交互更新

安装器打开后直接安装，展示解压、组件、服务、快捷方式等阶段，不再重复询问是否安装。Windows UAC 仍由系统控制。失败时可在安装窗口打开日志。

监控项目按硬件分组，OBS 与硬盘温度均可直接选择；游戏与排除使用连续列表，游戏历史使用单局统计卡片及详情报告。侧栏底部显示版本。游戏候选列表自动隐藏常见非游戏程序，手动添加优先于自动规则，未运行的手动游戏也保留在列表中。手动设为游戏会解除同名排除；手动排除会移除同名游戏规则。排除区仅显示用户自己的规则。

游戏历史每行提供删除，立即删除本地记录；删除当前会话后，本次会话不再保存，下一次启动游戏仍会正常记录。
