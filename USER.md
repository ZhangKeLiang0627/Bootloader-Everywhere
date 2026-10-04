# USER.md — 快速上手

> 这是给**使用者**的文档。想了解原理细节或接手开发，看 `AGENTS.md`。

## 这是什么

一个 STM32 串口 IAP Bootloader。它的核心能力是**防变砖**——就算刷进去一个「校验全对、但一跑就崩」的坏固件，它也能自动回滚，不会把你的板子刷死。

---

## 快速上手（3 步刷机）

1. 用 **Chrome / Edge** 打开网页：**https://zhangkeliang0627.github.io/Bootloader-Everywhere/**
2. 点「连接串口」，选你的板子串口（115200 8N1）
3. 拖入 `.bin` 固件，点「开始升级」

网页会自动把板子唤回 Bootloader、开始传输，全程不用按任何键、不用手动复位。

> 注意：固件（.bin）必须编译到 **0x08004000** 起始（Bootloader 占了前 16KB）。

---

## 跳转原理（一句话版）

上电后 Bootloader 先跑，读配置区判断固件状态：固件健康就跳过去，不健康（空片/坏固件/正在升级）就留在 Bootloader 等刷机。

```
上电 → Bootloader 启动 → 读固件状态 → 健康? → 跳转 APP（0x08004000）
                                      └→ 不健康 → 留在 IAP 等 YMODEM 刷机
```

---

## 核心机制（三种「防变砖」手段）

| 机制 | 干什么 | 一句话 |
|---|---|---|
| **自确认** | 刷完固件先进入「Testing 观察态」，靠看门狗判断新固件活没活下来 | 上电/按复位活下来了 → 转 Valid |
| **回滚** | Testing 态连续 3 次被看门狗拉回来（跑飞）→ 自动作废，退回 IAP | 坏固件自动打回，不会卡死 |
| **唤回窗口** | APP 运行中想刷机，网页发 `#Bootloader-Everywhere`，APP 软复位进 15s 窗口等刷机 | 不用拆机、不用抢上电时间 |

---

## ⚠️ 看门狗：APP 必须喂狗

Bootloader 默认开启了看门狗（IWDG），**它一旦启动就关不掉**。所以：

- **你的 APP 必须持续喂狗**（写 `IWDG_KR = 0xAAAA`，主循环里做就行）
- 不用初始化看门狗（Bootloader 已经启动了），只需喂
- 不喂狗 → 6 秒复位一次，最终被当成「跑飞的坏固件」回滚

如果你的 APP 实在不想用看门狗，改 `Bootloader/bl_config.h`：`BL_USE_WATCHDOG` 和 `BL_BOOT_SELF_CONFIRM` 都设成 `0`（代价是失去自动回滚）。

---

## 你的 APP 侧需要做什么（两件事）

1. **喂狗**（必须）：
   ```c
   IWDG->KR = 0xAAAA;   // 主循环里周期调用
   ```
2. **响应唤回**（可选，想网页一键刷机才需要）：检测串口收到 `#Bootloader-Everywhere` 后软复位。直接 `#include "bl_app.h"` 调 `bl_request_update()`，或照 `TestApp/app_main.c` 的状态机样板自己写。

---

## 工具位置

| 工具 | 位置 | 用途 |
|---|---|---|
| 网页上位机 | `docs/`（web 分支） | 浏览器刷机，已上线 |
| 命令行构建 | `Bootloader/tools/build.py` | 编译 + 量 ROM + 导出 bin/hex |
| 板端工具 | `Bootloader/tools/board.py` | 备份/烧写/擦除/看串口 |
| 测试固件编译 | `TestApp/build_app.py` | 编正常/故障测试固件 |
| 自动化测试 | `TestApp/tools/test_auto.py` | 一键跑回滚/升级/断流测试 |
| 移植指南 | `Bootloader/docs/PORTING.md` | 换芯片怎么移植 |

---

## 三个分支

| 分支 | 是什么 |
|---|---|
| `main` | Bootloader 库 + STM32F401 工程 |
| `test-app` | 库 + 测试 APP + 测试脚本 |
| `web` | 网页上位机（改完 push 自动更新线上页面） |

---

## 常用命令速查

```bash
# 编译 bootloader（Keil 命令行）
UV4 -r MDK-ARM/LUMOS-bootloader.uvprojx -j0 -o build.log

# 编译 + 下载到板子
UV4 -f MDK-ARM/LUMOS-bootloader.uvprojx -j0 -o flash.log

# 命令行量 ROM（不打开 Keil）
python Bootloader/tools/build.py

# 看板子串口
python Bootloader/tools/board.py monitor

# 编译测试固件（在 test-app 分支）
python TestApp/build_app.py --fail 0
```
