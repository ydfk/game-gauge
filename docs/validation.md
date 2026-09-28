# 实机验收记录

本页用于 Windows 11、双屏 4K HDR 和 OBS 窗口采集的实测。看到 `capture.accepted=true` 只能说明 Windows 接受了排除请求，不能替代录制文件检查。

## 准备

1. 在管理员 PowerShell 中安装 Intel 官方签名的 PresentMon MSI：`./scripts/install-presentmon.ps1 -MsiPath <MSI 路径>`。先运行 `-VerifyOnly` 可以只核对安装包，不修改系统。
2. 启动 `GameGauge.exe` 和一款已知可录制的无边框或窗口化游戏。优先使用实际游戏；源码构建产生的 `GameGauge.RenderProbe.exe` 只用于 DX11 帧链路排错，便携包不包含此工具。
3. 在“设备与游戏”确认游戏目标和所选 GPU。`GameGauge.Diagnostics.exe --host-status` 的 `snapshot.fps.state` 应为 `valid`，`snapshot.frame_samples` 应持续增加；CPU/GPU/内存读数要有合理来源。
   先执行 `GameGauge.Diagnostics.exe --targets` 找到游戏 PID，再运行 `GameGauge.Diagnostics.exe --presentmon-probe <PID> --sample-ms 10000`；保存 `raw_frames` 和 `accepted_frames`。探针没有有效帧时退出码为 2。若两者均为 0，应先核对 Intel PresentMon 自带工具能否读取同一游戏，再排查本项目的查询链路。
   `GameGauge.Diagnostics.exe --presentmon-capabilities` 可保存各设备的指标能力；`not_implemented_by_presentmon` 不能解释为设备温度为 0。
4. 记录 Windows 构建、显卡驱动、OBS 版本、游戏版本、屏幕分辨率与缩放、HDR 开关、捕获源和捕获方法。

## OBS 排除验证

在真实游戏中打开顶部监控条，确认玩家屏幕上可见，且不吞鼠标和键盘输入。分别添加 OBS 的游戏窗口采集源，选择“自动”捕获方法并录制 20 秒；如 OBS 提供 Windows Graphics Capture 和 BitBlt 方法，再各录制 20 秒。停止录制后打开**实际文件**，检查监控条是否完全消失，以及有无黑块、透明洞、闪烁或错误裁切。对显示器采集和游戏采集重复一次，逐项记录结果，不能用其他采集源的通过代替窗口采集通过。

| 环境/场景 | 玩家屏幕有 HUD | OBS 文件无 HUD | 无黑块/闪烁 | FPS 有效 | 备注 |
| --- | --- | --- | --- | --- | --- |
| 4K HDR 主屏、无边框、窗口采集自动 | 待测 | 待测 | 待测 | 待测 | |
| 4K HDR 主屏、窗口采集 WGC | 待测 | 待测 | 待测 | 待测 | |
| 4K HDR 主屏、窗口采集 BitBlt | 待测 | 待测 | 待测 | 待测 | |
| 第二块屏幕、窗口化游戏 | 待测 | 待测 | 待测 | 待测 | |
| 游戏采集与显示器采集 | 待测 | 待测 | 待测 | 待测 | 分开记录 |

还应测试 Alt+Tab、游戏分辨率变更、HDR 切换、睡眠恢复、Explorer 重启、游戏退出后的悬浮条和托盘状态。每次仅保留必要的录制文件用于本地核验；导出的诊断 JSON 可能包含游戏可执行路径，公开分享前先检查。

当前自动化验证仅覆盖构建、核心契约、隔离宿主 IPC 和配置持久化。没有真实游戏与 OBS 实际录制文件时，上表保持“待测”。
