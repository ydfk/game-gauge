# 实机验收记录

本页用于 Windows 11、双屏 4K HDR 和 OBS 采集的实测。看到 `capture.accepted=true` 只能说明 Windows 接受了排除请求，不能替代录制文件检查。当前按用户最新选择先验收游戏采集，窗口采集随后单独验收。

## 准备

1. 在管理员 PowerShell 中安装 Intel 官方签名的 PresentMon MSI：`./scripts/install-presentmon.ps1 -MsiPath <MSI 路径>`。先运行 `-VerifyOnly` 可以只核对安装包，不修改系统。
2. 启动 `GameGauge.exe` 和一款已知可录制的无边框或窗口化游戏。优先使用实际游戏；源码构建产生的 `GameGauge.RenderProbe.exe` 只用于 DX11 帧链路排错，便携包不包含此工具。
3. 在“设备与游戏”确认游戏目标和所选 GPU。`GameGauge.Diagnostics.exe --host-status` 的 `snapshot.fps.state` 应为 `valid`，`snapshot.frame_samples` 应持续增加；CPU/GPU/内存读数要有合理来源。
   先执行 `GameGauge.Diagnostics.exe --targets` 找到游戏 PID，再运行 `GameGauge.Diagnostics.exe --presentmon-probe <PID> --sample-ms 10000`；保存 `raw_frames` 和 `accepted_frames`。探针没有有效帧时退出码为 2。若两者均为 0，应先核对 Intel PresentMon 自带工具能否读取同一游戏，再排查本项目的查询链路。
   `GameGauge.Diagnostics.exe --presentmon-capabilities` 可保存各设备的指标能力；`not_implemented_by_presentmon` 不能解释为设备温度为 0。
4. 记录 Windows 构建、显卡驱动、OBS 版本、游戏版本、屏幕分辨率与缩放、HDR 开关、捕获源和捕获方法。

## OBS 排除验证

在真实游戏中打开顶部监控条，确认玩家屏幕上可见，且不吞鼠标和键盘输入。先按当前用户习惯用 OBS 游戏采集录制，再分别用窗口采集的自动、Windows Graphics Capture、BitBlt 方法录制 20 秒（以当前 OBS 提供的方法为准）。停止录制后打开**实际文件**，检查监控条是否完全消失，以及有无黑块、透明洞、闪烁或错误裁切。显示器采集也单独记录；一种采集源的结果不能代替其他采集源。

| 环境/场景 | 玩家屏幕有 HUD | OBS 文件无 HUD | 无黑块/闪烁 | FPS 有效 | 备注 |
| --- | --- | --- | --- | --- | --- |
| 4K HDR 主屏、无边框、窗口采集自动 | 待测 | 待测 | 待测 | 待测 | |
| 4K HDR 主屏、窗口采集 WGC | 待测 | 待测 | 待测 | 待测 | |
| 4K HDR 主屏、窗口采集 BitBlt | 待测 | 待测 | 待测 | 待测 | |
| 第二块屏幕、窗口化游戏 | 待测 | 待测 | 待测 | 待测 | |
| 4K HDR 主屏、《控制：共振》、游戏采集 | 用户确认无边框与全屏可见 | 用户确认录屏中无 HUD | 待检查文件 | 通过 | OBS 32.2.2；D3D12 游戏采集。录制文件 `E:\OBS\2026-09-28_20-19-48.mkv`，20:19:48–20:21:34；文件由 OBS 正常结束，HUD 排除结论来自用户目视反馈，尚未由本项目独立逐帧检查。录制时交换链报告 `Windowed: 1`，不能据此推断全屏模式也已录制验收。 |
| 显示器采集 | 待测 | 待测 | 待测 | 待测 | 与游戏采集分开记录 |

## 2026-09-28 真实游戏帧流

- 目标游戏为 `CONTROLResonant.exe`。独立 PresentMon 探针采样 10 秒得到 `raw_frames=1862`、`accepted_frames=1862`，`fps.state=valid`，当次 FPS 约 218。隔离宿主随后也曾报告有效 FPS，约 63.6；帧率随场景与前台状态变化，两个数值不作性能对比。
- 隔离宿主的 `capture.requested=true`、`capture.accepted=true`、`capture.error=0`，`hud_error` 为空。用户明确澄清“无边框和全屏 HUD 都能看到，OBS 录屏里面没有”，因此本轮游戏采集的 HUD 显示/排除满足用户预期。OBS 日志确认录制完成，但此处不把 Windows 接受排除请求当成独立视频检查。
- 失焦时 FPS 转为 `waiting`、有效统计暂停；这符合当前会话规则。窗口采集、显示器采集、黑块/闪烁、输入穿透和 HDR 视觉质量仍待逐项验收。

## 2026-09-28 原生 CPU 温度探针

- 从 [PawnIO.Modules 官方 0.2.11 发行版](https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.11)取得 `release_0_2_11.zip`，校验发行资产 SHA-256 为 `43608cb89bc84247fef1368a139013f7d043e17db6d6c8dfc9b46bf0905a81f4`；其中 `AMDFamily17.bin` 的 SHA-256 为 `dae74615761b78bdf064dfb3e136252ddcc6fc727d88f14738d0e5800d427a91`。模块留在 Git 忽略的 `.deps`，不进入便携包。
- `GameGauge.CpuProbe.exe` 限定上述模块哈希，仅调用 PawnIO 的 `ioctl_read_smn` 读取 SMN `0x59800`，并按 [Linux k10temp 驱动](https://github.com/torvalds/linux/blob/master/drivers/hwmon/k10temp.c)的温度位与 49°C 修正规则解码。此探针是单次技术验证程序，尚未作为宿主的长期采样后端。
- 普通权限连接已运行的 PawnIO 驱动返回 `HRESULT=0x80070005`（拒绝访问）；管理员 PowerShell 中运行成功，本机 CPU family/model 为 `0x1A/0x44`，寄存器原始值 `0x72db0000`，得到 `Tctl=65.75°C`。这是本机单次有效读数；还需与独立传感器软件在相同时间段对照，并验证持续采样、权限隔离、其他 CPU 型号和发行许可。
- `GameGauge.CpuProbe.exe` 仅随源码构建生成。运行命令：`./build/core/bin/Release/GameGauge.CpuProbe.exe ./.deps/AMDFamily17-0.2.11.bin`（管理员 PowerShell）。正式产品不应为了温度让整个 HUD/设置进程提权，后续应通过受限的独立传感器进程提供读数。

还应测试 Alt+Tab、游戏分辨率变更、HDR 切换、睡眠恢复、Explorer 重启、游戏退出后的悬浮条和托盘状态。每次仅保留必要的录制文件用于本地核验；导出的诊断 JSON 可能包含游戏可执行路径，公开分享前先检查。

自动化验证覆盖构建、核心契约、隔离宿主 IPC 和配置持久化；上面的真实游戏与 OBS 结果属于本机实测和用户反馈，尚未扩展到其他机器或采集方法。
