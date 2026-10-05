# Bootloader-Everywhere

串口 IAP Bootloader，做成**可搬走的单文件库**，外加一个能一键刷机的网页上位机。
任何时刻断电都不会变砖。

**在线刷机页：<https://zhangkeliang0627.github.io/Bootloader-Everywhere/>**
（Chrome / Edge，用 Web Serial，不装任何软件）

---

## 一句话说明它怎么工作

上电只看 APP 区最前面的两个字（栈顶 SP + 复位入口 PC），而这两个字是
**升级过程中最后才写进去的** —— 所以"这两个字合法"就等于"上一份固件完整刷完了"。
传输中断、掉电、断流，这两个字都不会落盘，板子就停在 IAP 等你重刷。

不需要配置区、不需要状态标志、不需要看门狗。细节见
[`Bootloader/README.md`](Bootloader/README.md)。

---

## 从哪里开始

| 你想做什么 | 看这里 |
|---|---|
| **想用这个库 / 理解它** | [`Bootloader/README.md`](Bootloader/README.md) ← 从这里开始 |
| 想把它移植到别的芯片 | 同上，「移植（换芯片）」一节 |
| 想改 Flash 分区 / 超时 | [`Bootloader/bl_config.h`](Bootloader/bl_config.h) |
| 想改协议 / 升级流程 | [`Bootloader/bl.cpp`](Bootloader/bl.cpp) |
| 想用网页刷机 | 上面的在线地址，或 web 分支的 `docs/` |
| 想跑自动化/断电测试 | `TestApp/tools/`（test-app 分支） |
| **接手开发（人或 AI）** | [`AGENTS.md`](AGENTS.md) |
| 只想快速上手用一下 | [`USER.md`](USER.md) |

---

## 仓库里有什么

```
Bootloader/        ★ 就是这个库，5 个文件，没有子目录
tools/             开发工具：编译量体积（build.py）、板端操作（board.py）
AGENTS.md          给接手的人 / AI 的完整说明
USER.md            给使用者的快速上手

—— 以下是 STM32F401 示例工程，不是库的一部分 ——
Core/ Drivers/ MDK-ARM/ Bootloader-Everywhere.ioc
                   CubeMX + Keil 工程（含宿主 main），演示"库怎么接进真实工程"
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
