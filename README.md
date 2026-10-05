# Bootloader-Everywhere

可搬走的串口 IAP Bootloader，外加一个能一键刷机的网页上位机。

**在线刷机页：<https://zhangkeliang0627.github.io/Bootloader-Everywhere/>**
（Chrome / Edge，用 Web Serial，不装任何软件）

---

## 从哪里开始

| 你想做什么 | 看这里 |
|---|---|
| **想用这个库 / 理解它** | [`Bootloader/README.md`](Bootloader/README.md) ← 从这里开始 |
| 想把它移植到别的芯片 | 同上，「移植到别的芯片」一节 |
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
