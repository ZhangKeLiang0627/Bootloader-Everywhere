# USER.md — 快速上手

> 这是给**使用者**的文档。想了解原理细节或接手开发，看 `AGENTS.md`。

## 这是什么

一个可移植的**串口 IAP Bootloader**。板子先跑它，它再决定是跳进你的
APP、还是留在原地等新的固件。

- 用 YMODEM-1K 协议经串口刷固件，写入 APP 区 → 校验 → 跳转
- 有网页版上位机（Web Serial）：只要浏览器，不用装任何软件
- **定位是 IAP，不是 OTA** —— 人站在板子前面刷，所以不做自动回滚、
  不启用看门狗，追求的是简单、可移植、可读懂

---

## 快速上手（3 步刷机）

1. 用 **Chrome / Edge** 打开网页：
   **https://zhangkeliang0627.github.io/Bootloader-Everywhere/**
2. 点「连接串口」，选你的板子串口（115200 8N1）
3. 拖入 `.bin` 固件，点「开始升级」

网页会自动把板子唤回 Bootloader 再开始传输 —— **全程不用按键、不用手动复位**。

> ⚠️ 固件（.bin）必须编译到 **0x08004000** 起始（Bootloader 占了前 16KB）。
> 起始地址不对，刷进去也跑不起来。

---

## 跳转原理（一句话版）

上电后 Bootloader 先跑，读配置区判断固件状态：可跳转就跳过去，不可跳转
（空片 / 正在升级 / 写到一半掉电）就留在 IAP 等刷机。

```
上电 → Bootloader 启动 → 读固件状态 → Valid? → 跳转 APP（0x08004000）
                                    └→ 否    → 留在 IAP 等 YMODEM 刷机
```

固件状态只有三个：

| 状态 | 什么时候 | 结果 |
|---|---|---|
| `Invalid` | 配置区空 / 没刷过 | 留在 IAP |
| `Download` | 正在写，或写到一半掉电 | 留在 IAP（可重刷，**不会变砖**） |
| `Valid` | 写完且校验通过 | 跳转 APP |

---

## 运行中的 APP 怎么回去刷机（唤回窗口）

老办法是"上电 300ms 内按某个键"，既慢又难卡准，已经废掉。现在是：

1. 网页点「开始升级」→ 向串口逐字节发关键字 `#Bootloader-Everywhere`
2. APP 匹配到完整关键字 → 自己做一次软件复位
3. Bootloader 读复位原因，发现是「软件复位 + 固件 Valid」→ 开一个
   **15 秒的限时窗口**等 YMODEM
4. 窗口内收到首包就正常刷机；15 秒没等到东西，自动跳回 APP 继续跑
5. 正常上电 / 按复位键 → **零等待**直接跳 APP，没有任何启动延迟

---

## 你的 APP 侧需要做什么

**只有一件可选的事**：想支持"网页一键刷机"，就在串口收齐关键字后软复位。

```c
#include "bl.h"            /* 只用到 bl_request_update，零依赖 */

/* 在你的串口接收处理里，逐字节匹配关键字，匹配完整后调用： */
bl_request_update();       /* 写 SCB->AIRCR = SYSRESETREQ，软复位 */
```

样板见 `TestApp/app_main.c`（test-app 分支，纯寄存器实现）。
**不需要**做什么"确认"、"喂狗"之类的动作 —— 本 Bootloader 不要这些。

---

## 已知取舍（说清楚，免得踩坑）

- **刷进坏固件不会自动回滚**。能过 CRC 校验、但一跑就崩的固件，会被正常
  跳转进去然后卡住；这时断电重上电，Bootloader 会回 IAP，重新刷一个即可。
  这是刻意去掉的复杂度，不是缺陷。
- **没有看门狗**。所以 APP 不需要（也无需）喂狗。
- 传输中断电是安全的：状态停在 `Download`，下次上电留在 IAP 可重刷。

---

## 工具位置

| 工具 | 位置 | 用途 |
|---|---|---|
| **库本体（只需这 6 个文件）** | `Bootloader/` | bl.h / bl.cpp / bl_port.h / bl_port_stm32f4.cpp / bl_config.h / README.md |
| 库文档（含移植指南） | `Bootloader/README.md` | 怎么用、怎么移植、常见坑 |
| 网页上位机 | `docs/`（web 分支） | 浏览器刷机，已上线 |
| 命令行构建 | `tools/build.py` | 编译 + 量 ROM + 导出 bin/hex |
| 板端工具 | `tools/board.py` | 备份/烧写/擦除/看串口 |
| 测试固件编译 | `TestApp/build_app.py` | 编正常/故障测试固件（test-app 分支） |
| 自动化测试 | `TestApp/tools/test_auto.py` | 一键跑升级/唤回/断流测试 |
| 断电测试引导 | `TestApp/tools/power_test.py` | 提示你拔电的断电暴力测试 |
| 历史资料 | `docs/HISTORY.md` | 立项期测试方案与调研（不代表当前实现） |

> **移植到别的工程时，只拷 `Bootloader/` 这一个目录就够** —— 仓库里的
> `Core/ Drivers/ MDK-ARM/` 只是 STM32F401 的示例工程。

---

## 三个分支

| 分支 | 是什么 |
|---|---|
| `main` | Bootloader 库 + STM32F401 示例工程 |
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
python tools/build.py

# 看板子串口
python tools/board.py monitor

# 编译测试固件（在 test-app 分支）
python TestApp/build_app.py --fail 0
```
