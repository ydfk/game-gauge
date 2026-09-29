# 第三方依赖

| 组件 | 用途 | 许可与分发 |
| --- | --- | --- |
| nlohmann/json 3.12.0 | JSON | MIT，源码树 third_party/nlohmann，安装包 licenses/nlohmann.txt |
| Intel PresentMon 2.6.0 | SDK 与帧采集服务 | MIT；安装器附带未修改的 Intel 签名 MSI，许可证位于 licenses/PresentMon.txt |
| PawnIO 2.2.0 / PawnIOLib | 驱动访问 | 官方签名安装程序原样再分发；其安装程序明确允许未修改再分发；开源版与官方二进制许可不同 |
| PawnIO.Modules 0.2.11 AMDFamily17 | AMD Tctl 只读采集 | 官方模块原样内置，固定 SHA-256；官方文档允许把发行模块包含在软件中 |

官方来源：
- https://github.com/GameTechDev/PresentMon/releases/tag/v2.6.0
- https://github.com/namazso/PawnIO.Setup/releases/tag/2.2.0
- https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.11
- https://github.com/namazso/PawnIO.Modules/wiki/Using-PawnIO-Modules

PawnIO 安装器 SHA-256：1f519a22e47187f70a1379a48ca604981c4fcf694f4e65b734aaa74a9fba3032。
AMD 模块 SHA-256：dae74615761b78bdf064dfb3e136252ddcc6fc727d88f14738d0e5800d427a91。
PresentMon MSI SHA-256：0cb43d2622356c1e277a77f840887f9000cea4e2d4236c0850922fc204c6f820。

安装器使用默认受限 PawnIO，不启用 unrestricted 版本。温度模块固定且经校验，服务没有接受任意寄存器操作的 IPC。项目不修改、重新签名或单独抽出分发官方驱动。

assets/logo-concept.png 是原创描述生成的概念图；assets/logo.svg 是本项目矢量图。LibreHardwareMonitor 仅用于算法对照，未加入产品。