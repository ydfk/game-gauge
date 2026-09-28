# 第三方依赖

| 组件 | 用途 | 许可与分发 |
| --- | --- | --- |
| nlohmann/json 3.12.0 | C++ JSON 序列化 | MIT；源码位于源码树 `third_party/nlohmann/`，便携包许可证位于 `licenses/nlohmann-json-MIT.txt` |
| Intel PresentMon SDK 2.6.0 | 帧事件 API 声明 | MIT；头文件位于源码树 `third_party/presentmon/`，便携包许可证位于 `licenses/PresentMon-SDK-MIT.txt` |
| Intel PresentMon Shared Service | FPS 运行时服务 | 用户从 Intel 正式 MSI 单独安装；本工程不再分发其服务二进制 |
| PawnIO 2.0.1.0 / PawnIOLib | 源码构建的单次 CPU 温度探针通过已安装的 DLL 连接驱动 | 用户单独安装；便携包不包含 DLL 或驱动。官方二进制许可和开源版本许可不同，正式分发前继续审查：[官方使用与许可说明](https://github.com/namazso/PawnIO.Modules/wiki/Using-PawnIO-Modules) |
| PawnIO.Modules 0.2.11 `AMDFamily17.bin` | 单次 AMD SMN 温度读取 | 从[官方发行版](https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.11)另行取得、校验 SHA-256 并放在 Git 忽略目录；不纳入源码树或便携包。模块源码为 LGPL-2.1-or-later，正式分发前继续核对二进制许可 |

`assets/logo-concept.png` 是本项目根据原创描述生成的概念图；`assets/logo.svg` 是本项目绘制的矢量母版。研究目录 `.deps/` 不纳入发布包。PawnIO 目前仅用于单次诊断探针，常驻产品仍未接入；LibreHardwareMonitor 资料仅用于算法对照，未加入产品。
