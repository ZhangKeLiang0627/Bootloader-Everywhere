# Bootloader —— 可移植串口 IAP 引导程序

一个与芯片无关、可移植到 STM32 / GD32 / CH32 等 Cortex-M 平台的串口 IAP Bootloader。
升级协议采用 **YMODEM-1K**（公共协议，SecureCRT / Xshell / MobaXterm / Tera Term / lrzsz 原生支持）。

---

## 设计目标

1. **绝不变砖**：任何时刻掉电，设备都能恢复。见下方"防变砖设计"。
2. **平台无关**：核心逻辑不含任何寄存器操作，移植只改 `port/` 与 `target/<chip>/`。
3. **协议通用**：不自己发明协议，用 YMODEM，PC 端不需要专门写上位机。

---

## 目录结构

```
Bootloader/
├── README.md                 本文档
├── config/
│   └── bl_config.h           ★ 分区地址、功能开关（移植时改这里）
├── core/                     平台无关逻辑（C++，移植时不动）
│   ├── bl_types.hpp          公共类型：Status / FwState / IapResult
│   ├── bl_crc.hpp/.cpp       Crc16（YMODEM 用）与 Crc32（镜像校验）
│   ├── bl_meta.hpp/.cpp      配置区：固件状态机、日志式槽位读写
│   ├── bl_verify.hpp/.cpp    镜像合法性检查（向量表 + CRC32）
│   ├── bl_ymodem.hpp/.cpp    YMODEM-1K 接收端
│   ├── bl_session.hpp/.cpp   升级会话：原子提交流程编排
│   └── bl_boot.hpp/.cpp      上电启动决策
├── port/
│   ├── bl_port.hpp           ★ 移植接口定义（新平台实现这组函数）
│   └── bl_port_template.cpp  移植模板（填空参考，勿加入编译）
├── target/
│   └── stm32f4/              本工程目标平台
│       ├── bl_port_flash_stm32f4.cpp
│       ├── bl_port_uart_stm32f4.cpp
│       └── bl_port_system_stm32f4.cpp
└── app/
    └── bl_main.cpp           入口与 IAP 主循环
    └── bl_main.c             Bootloader 主流程
```

**分层原则**（参考 OpenBLT 的四层解耦）：
`app/` 是主流程 → `core/` 是平台无关的大脑 → `port/bl_port.h` 是契约 → `target/` 是落地实现。
新平台移植时：**只新增一个 `target/<chip>/` 目录 + 改 `config/bl_config.h`**，`core/` 一行不动。

---

## 防变砖设计

这是本项目的第一优先级需求。全部措施如下：

### 1. Bootloader 自身不可被覆盖
Bootloader 常驻 `S0`（16KB），升级流程**只操作 APP 区与配置区**，
永远不会擦写自己。因此无论 APP 区被破坏成什么样，重刷入口永远存在。

### 2. 原子提交（顺序是安全的核心）
升级严格按以下顺序执行：

```
① 配置区写 fw_state = DOWNLOAD      ← 必须先置无效
② 擦除 APP 区所需扇区
③ 接收 YMODEM 数据并写入
④ 整镜像 CRC32 校验
⑤ 校验通过 → fw_state = TESTING，记录 size / crc32 / 版本
⑥ 跳转 APP
```

**关键**：第①步必须在第②步之前。如果顺序写反（先擦 APP 再置标志），
擦除或传输中途掉电时，配置区仍停留在 `VALID`，但 APP 已经是空片
（全 `0xFF`），上电后 Bootloader 会判定"固件有效"并跳进空片 → **变砖**。

按上述顺序，任意阶段掉电的结果都是安全的：

| 掉电时刻 | 配置区状态 | 上电行为 |
|---|---|---|
| 置 DOWNLOAD 之前 | `INVALID` / `VALID` | 正常（原有固件未动） |
| 置 DOWNLOAD 之后 | `DOWNLOAD` | 停在 IAP，可重刷 |
| 擦除 APP 中途 | `DOWNLOAD` | 停在 IAP，可重刷 |
| 传输数据中途 | `DOWNLOAD` | 停在 IAP，可重刷 |
| CRC 校验失败 | `DOWNLOAD` | 停在 IAP，可重刷 |

### 3. 试运行与自动回滚（应对"能通过校验但跑不起来"）
固件 CRC 正确不代表逻辑正确（可能初始化就 HardFault）。
因此新固件先置于 `TESTING` 态：

- 每次启动，Bootloader 把 `boot_attempts` 加一后写回配置区，再跳转 APP；
- APP 启动成功并完成自检后，调用 `bl_meta_confirm_app()` 把状态置 `VALID`、计数清零；
- 若连续 `BL_BOOT_MAX_ATTEMPTS`（默认 3）次都没等到 APP 确认，
  判定该固件有运行时缺陷 → 置 `INVALID`，停在 IAP 等重刷。

这相当于给"能通过校验但会崩"的固件加了一道保险。

### 4. 看门狗
Bootloader 全程开启 IWDG。擦除一个 128KB 扇区典型耗时约 1s（最大 4s），
在所有长耗时操作的循环中喂狗。避免 Bootloader 自身卡死后设备彻底失去响应。

### 5. Backdoor 强制进入
上电后在 `BL_BACKDOOR_WINDOW_MS`（默认 300ms）时间窗内监听串口，
收到 `BL_BACKDOOR_CHAR`（默认 `0x7F`）即强制进入 IAP，忽略固件状态。
用于 APP 正常但需要强制升级的场景，也是开发期的救命通道。

---

## 配置区（Flash 参数区）

采用**日志式槽位轮转**，避免每次启动都擦扇区：

- 整个配置扇区（`S11`，128KB）划分为 `2048` 个 `64` 字节槽位；
- 每次状态变更**顺序追加**写一个新槽，不擦除；
- 槽内带自增序号与 CRC32，读取时从后向前扫描，取序号最大的有效槽；
- 写满整个扇区后才擦除一次并从头开始。

好处：常规状态写入只有 64 字节，耗时几十微秒，**不会给启动引入延迟**；
且擦写寿命被槽位轮转摊薄。

---

## 移植指南（以 GD32 / CH32 为例）

1. 复制 `target/stm32f4/` 为 `target/gd32f30x/`（或 `ch32v307/`）；
2. 在 `config/bl_config.h` 中改三个地址（`BL_BOOT_BASE` / `BL_APP_BASE` / `BL_META_BASE`）
   与 Flash 容量，**必须与目标芯片的扇区边界对齐**；
3. 实现 `port/bl_port.h` 里的全部函数（可参考 `bl_port_template.c` 的注释）：
   - Flash：初始化 / 查扇区大小 / 擦除 / 编程 / 读取
   - UART：初始化 / 读 / 写 / 清接收缓存
   - 系统：跳转 APP / 喂狗 / 取毫秒时基
4. 把 `target/<chip>/*.c` 与 `core/*.c`、`app/bl_main.c` 加入编译；
5. 注意目标芯片的 Flash 编程粒度（F4 是 32 位字，部分 GD32/CH32 是半字或页），
   在 `bl_port_flash_write()` 内部处理对齐与补齐，`core/` 不关心。

### 移植时必须核对的三件事

- **扇区/页边界**：`BL_APP_BASE` 与 `BL_META_BASE` 必须落在扇区起始地址上，
  否则擦除会误伤相邻区域。各系列页大小不同（F1 中容量 1KB / 大容量 2KB，
  F4 为不等长扇区），务必查对应参考手册的 Flash 章节。
- **编程粒度与对齐**：地址需按编程单位对齐，写长度需为编程单位的整数倍；
  不足部分在移植层补齐（通常补 `0xFF`）。
- **擦除后的值**：正常为 `0xFF`。核心层依赖这一点判断槽位是否为空。

---

## YMODEM 交互流程

```
Bootloader                     PC 上位机
    |                               |
    |  <------ 'C' (0x43) ----------|  请求 CRC 模式
    |                               |
    |  ---- SOH 0 帧（文件名+大小）->|  首包
    |  [此时按 size 一次性擦除所需扇区]
    |  ------- ACK + 'C' ---------->|
    |                               |
    |  <---- STX 1 + 1024B + CRC ---|  数据包（停等）
    |  --------- ACK -------------->|
    |           ...                 |
    |  <---------- EOT -------------|
    |  ------- ACK + 'C' ---------->|
    |  <---- SOH 0 帧（全零）-------|  批次结束
    |  --------- ACK -------------->|
    |                               |
    |  [校验整镜像 CRC32 → 提交状态]
```

**擦除时机**：擦除在所有数据到达之前完成（收到首包的 size 后立刻做），
这样擦除耗时发生在 PC 端等待 ACK 的窗口内，不会触发上位机超时重传。
若边收边擦，128KB 扇区 1s 的擦除会撑爆多数上位机的默认超时。

---

## 参考的开源项目

| 项目 | 采样了什么 |
|---|---|
| **OpenBLT**（Feaser） | 分层解耦架构（应用配置 / 核心逻辑 / 硬件抽象 / 通信）、Backdoor 入口机制、5-20KB 的 ROM 占用控制思路 |
| **MCUboot** | `image_ok` 确认语义、trial swap 与自动回滚、原子提交的元数据设计 |
| **ST AN2606 / AN4657** | STM32 系统 Bootloader 与 IAP 跳转的官方做法（关中断、清挂起、设 MSP、`__set_CONTROL(0)`） |

A/B 双分区虽是业界最主流的防变砖方案，但需要两份完整 APP 空间；
本芯片 1MB Flash 装不下（APP 区 880KB，双份需 1.76MB），
故采用"原地覆盖 + 状态机 + 自动回滚"，这是 Flash 受限场景下的经典折中。

---

## 分区表（LUMOS-Link / STM32F405RGT6）

| 区域 | 起始地址 | 大小 | 扇区 | 说明 |
|---|---|---|---|---|
| Bootloader | `0x08000000` | 16 KB | S0 | 常驻，不可覆盖 |
| APP | `0x08004000` | 880 KB | S1 - S10 | 应用程序 |
| 配置区 | `0x080E0000` | 128 KB | S11 | 固件状态、长度、CRC、启动计数 |

APP 侧工程需把 Keil 的 IROM1 起始地址改为 `0x08004000`、大小 `0xDC000`，
并在 `main()` 最开始调用 `SCB->VTOR = 0x08004000;`（或使用 `VECT_TAB_OFFSET`）。

---

## 编译环境

本工程用 C++ 编写，目标编译环境（已从工程文件确认）：

| 项 | 值 |
|---|---|
| 编译器 | ARM Compiler 6（AC6） |
| C++ 标准 | C++14 |
| RTTI | 已关闭 |
| 异常 | **建议关闭**（`-fno-exceptions`），省 ROM 且与代码风格一致 |

代码风格约定：`enum class` 强类型、`constexpr`、`static_assert`；
**不使用**异常、动态内存、RTTI、STL 容器。

## Keil 工程挂载步骤

1. 把 `core/`、`port/bl_port.hpp`、`target/stm32f4/`、`app/` 下的
   `*.cpp` 与 `*.hpp` 加入工程（**`bl_port_template.cpp` 不要加入**，
   它只是移植参考，会与 target 实现产生重复符号）。
2. Include 路径加上：`..\Bootloader\config`、`..\Bootloader\core`、
   `..\Bootloader\port`、`..\Bootloader\target\stm32f4`、`..\Bootloader\app`。
3. Target 页把 **IROM1 改为 `0x08000000` / `0x4000`**（16KB）。
4. Options → C/C++ → 勾选 **No Exceptions**。
5. **把 `UserApp/main.cpp` 从编译中移除**——原骨架里也定义了 `Main()`，
   与 `app/bl_main.cpp` 会重复符号。
6. 编译后查 `.map`，确认 **ROM 占用 < 16KB**。

## 一个设计决策：CRC32 由板子自己算

YMODEM 首包只带文件名和大小，不带整镜像 CRC32。本实现**不让上位机下发 CRC**，
而是由 Bootloader 在接收过程中自行计算并写入配置区。

理由：

- YMODEM 的**帧级 CRC16 + ACK/NAK 已经保证**了「PC → 板子」的传输正确性，这是协议本职；
- **整镜像 CRC32 的职责是防 Flash 位翻转 / 擦写不完整**，属于「本端自检」，
  只需算一次存下来，以后每次启动重算比对即可；
- 于是不需要约定「上位机用什么口径算 CRC」，**标准 YMODEM 工具（SecureCRT 等）
  无需任何改造就能用**；
- 也彻底避开了「末包填充字节是否计入 CRC」这类陷阱
  （不同工具的填充值可能是 `0x00` / `0x1A` / `0xFF`，详见 `bl_verify.cpp` 注释）。
