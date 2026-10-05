# Bootloader-Everywhere

一个可搬走的串口 IAP Bootloader，外加一个能一键刷机的网页上位机。

**在线刷机页：<https://zhangkeliang0627.github.io/Bootloader-Everywhere/>**
（Chrome / Edge，用 Web Serial，不用装任何软件）

---

## 从哪里开始

| 你想做什么 | 看这里 |
|---|---|
| **想用这个库 / 理解它** | [`Bootloader/README.md`](Bootloader/README.md) ← 从这里开始 |
| 想把它移植到别的芯片 | 同上，「移植到别的芯片」一节 |
| 想改分区 / 波特率 / 芯片型号 | [`Bootloader/bl_config.h`](Bootloader/bl_config.h) |
| 想改协议 / 升级流程 | [`Bootloader/bl.cpp`](Bootloader/bl.cpp) |
| 想用网页刷机 | 上面的在线地址，或 web 分支的 `docs/` |
| 想跑自动化/断电测试 | `TestApp/tools/`（test-app 分支） |
| **接手开发（人或 AI）** | [`AGENTS.md`](AGENTS.md) |
| 只想快速上手用一下 | [`USER.md`](USER.md) |

---

## 仓库里有什么

```
Bootloader/        ★ 就是这个库，6 个文件，没有子目录
tools/             开发工具：编译量体积（build.py）、板端操作（board.py）
docs/HISTORY.md    立项期的测试方案与调研（历史资料）
AGENTS.md          给接手的人/AI 的完整说明
USER.md            给使用者的快速上手

—— 以下是 STM32F401 的示例工程，不是库的一部分 ——
Core/ Drivers/ Bsp/ UserApp/ MDK-ARM/ LUMOS-bootloader.ioc
                   CubeMX + Keil 工程，用来说明"库怎么接进真实工程"
build/             编译产物
```

**只想用这个库的话，只拷 `Bootloader/` 就够了** ——
是库去配合你的工程，不是你的工程来配合这个仓库。

---

## 三个分支

| 分支 | 内容 |
|---|---|
| `main` | 库 + STM32F401 示例工程（当前） |
| `test-app` | 库 + 测试 APP + 测试脚本（`TestApp/`） |
| `web` | 网页上位机（`docs/`），push 后 GitHub Pages 自动重建 |

---

## 这个库是什么 / 不是什么

**是**：串口 IAP 刷固件（YMODEM-1K）、掉电安全的原地升级、APP 运行中可被唤回、
跨芯片可移植、十几 KB。

**不是**：OTA、A/B 双分区、自动回滚、看门狗监护。

后两项是**刻意去掉**的：定位是"人站在板子前刷"，刷坏了再刷一次即可，
不值得为它增加运行期副作用和一堆状态。详见 `Bootloader/README.md` 与
`AGENTS.md` 的「已删除的机制」。

---

## 许可

本仓库自行实现（YMODEM 是公开规范）。未直接复用 OpenBLT（GPLv3）或
ST 官方 IAP 例程（SLA0048）的代码，原因见 `docs/HISTORY.md`。
