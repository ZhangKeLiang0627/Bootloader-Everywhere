# 升级协议调研 与 项目实施清单

本文档回答三个问题：
1. OpenBLT 和 MCUboot 是怎么做串口 IAP 的？能不能直接拿来用？
2. 许可上哪些代码能用、哪些不能用？
3. 整个项目要做哪些事，怎么排？

---

## 一、升级协议选型（核心结论）

**结论先行：继续用 YMODEM。** 不是因为它更先进，而是因为另外两套方案的
"上位机成本"和"协议复杂度"都远高于它，而 YMODEM 的上位机是白送的。

### 1.1 OpenBLT 用的是 XCP 协议，不是 YMODEM/XMODEM

这是最容易误解的一点。OpenBLT **不用**任何 X/Y/ZMODEM，它用的是
**XCP v1.0（ASAM MCD-1 通用测量与校准协议）**——一个汽车电子领域的标准主从协议。
用于 IAP 时，一次升级的命令序列是：

| XCP 命令 | 作用 |
|---|---|
| `CONNECT` | 建立连接，目标返回协议版本与资源状态 |
| `GET_STATUS` | 查询 Bootloader 状态 |
| `SET_MTA` | 设置内存传输地址（下一个写入目标） |
| `GET_SEED` / `UNLOCK` | Seed/Key 挑战应答鉴权 |
| `PROGRAM_START` | 通知目标准备编程 |
| `PROGRAM_CLEAR` | 擦除指定 Flash 扇区 |
| `PROGRAM` | 分块写入固件数据（最耗时阶段） |
| `UPLOAD` | 回读内存，用于校验 |
| `PROGRAM_RESET` | 编程结束，复位或跳转 |

配套上位机：`MicroBoot`（GUI）、`BootCommander`（CLI）、`LibOpenBLT`（C 库）。
固件格式用 **S-record**（自带地址信息，比裸 bin 好）。

**它的架构确实漂亮**——四层解耦（应用配置 / 目标无关核心 / 目标相关驱动 / 编译器相关），
协议与传输层解耦（同一套 XCP 跑在 RS232 / CAN / CAN FD / USB / TCP/IP / Modbus 之上）。
但对我们来说有两个硬伤：**许可是 GPL（闭源要买商业授权）**，以及 **XCP 协议本身比 YMODEM 重得多**。

### 1.2 MCUboot 根本不做传输，它只管"镜像验证与切换"

这是第二个容易误解的点。MCUboot 是 `transport-agnostic` 的——
它只关心镜像槽位（primary / secondary）、镜像头（header）和尾部（trailer），
**完全不关心固件是怎么传进来的**。

它的串口升级走 **Serial Recovery** 功能，内部实现的是 **SMP（Simple Management Protocol）**
服务端。SMP 是 MCUmgr 管理协议的传输编码，数据用 **CBOR** 序列化。命令集是：

| 组 | 命令 | 作用 |
|---|---|---|
| OS (0) | `echo` | 回显测试 |
| OS (0) | `reset` | 复位设备 |
| IMG (1) | `image list` | 列出镜像槽状态 |
| IMG (1) | `image upload` | 上传固件到槽位 |

上位机是 `mcumgr` CLI（Go 语言写的，Apache-2.0）。
关键代码在 `boot/boot_serial/src/boot_serial.c`（SMP 服务端）和 `boot/zephyr/serial_adapter.c`（串口适配）。

**问题在于**：这套实现与 Zephyr 生态深度耦合——依赖 `flash_map`、设备树、
Zephyr 的工作队列与配置系统。往裸机 STM32 上移植，等于要把 SMP 服务端 + CBOR 编解码 +
串口适配整套搬过来，工作量远超自研一个 YMODEM 接收端。

### 1.3 三套方案横向对比

| 维度 | OpenBLT（XCP） | MCUboot（SMP/CBOR） | **YMODEM** |
|---|---|---|---|
| 下位机实现量 | ~1000-2000 行 | ~1500-2500 行（且耦合 Zephyr） | **~400 行** |
| 上位机 | MicroBoot / BootCommander | mcumgr CLI（Go 工具） | **串口工具原生支持，零开发** |
| 许可 | **GPL v3 或商业授权** | Apache-2.0 | 公开规范，无风险 |
| 协议复杂度 | 高（完整 XCP 栈） | 高（CBOR 编码 + 组/命令体系） | 低（定长帧 + CRC16） |
| 裸机移植难度 | 中（需移植 XCP slave） | **高**（需移植整个 SMP + CBOR） | 低 |
| 跨平台（GD32/CH32） | 许可受限 | 可用但搬运量大 | **无限制** |

**决定性差异是上位机**：OpenBLT 和 MCUboot 都要额外获取上位机工具
（一个受 GPL 约束，一个是 Go 工具链），而 YMODEM 的上位机就是你电脑上
已经装好的 SecureCRT / Xshell / MobaXterm / Tera Term，或者 Linux 下的 `sb` 命令。

### 1.4 因此：协议层继续用 YMODEM，但借鉴它们的设计

---

## 二、许可分析

| 来源 | 许可 | 能否直接用 |
|---|---|---|
| ST CubeF4 IAP 例程（`ymodem.c` / `flash_if.c`） | **SLA0048** | ❌ 仅限 ST 芯片 |
| **OpenBLT** | **GPL v3 / 商业双许可** | ❌ 闭源需付费授权 |
| **MCUboot** | **Apache-2.0** | ✅ 可用（但架构不匹配） |
| `zonque/xymodem-mini` | MIT | ⚠️ 只有发送端 |

### 2.1 ST 官方 IAP 例程：技术上现成，许可上不能用

路径（本机已有）：

```
C:\Users\11846\STM32Cube\Repository\STM32Cube_FW_F4_V1.27.1\
  Projects\STM324xG_EVAL\Applications\IAP\IAP_Main\
    src\ymodem.c      18930 B   ← YMODEM 收发完整实现
    src\flash_if.c     8537 B   ← Flash 擦除/编程封装，含 S0-S11 地址宏
```

`STM324xG_EVAL` 用的就是 **STM32F407**，与 F405 同系列同 Flash 布局，
`flash_if.h` 连扇区地址宏和 `APPLICATION_ADDRESS` 都写好了。**技术上零改动可用。**

但 Cube 包根目录 `Package_license.md` 声明的是 **SLA0048**，第 4 条原文：

> ...**must be used and execute solely and exclusively on or in combination with**
> microcontroller or microprocessor device manufactured by or for STMicroelectronics

即**只能在 ST 芯片上运行**。移植 GD32 / CH32 违反此条款。

> 说明：`flash_if.c` 等文件头部声明的是
> "licensed under terms that can be found in the LICENSE file in the root directory"，
> 指向的就是包根目录那份 SLA0048，最终仍受第 4 条约束。

### 2.2 OpenBLT：双重许可，默认 GPL

官网原文（`github.com/feaser/openblt`）：

> OpenBLT is offered under a **dual licensing model**. The default license is the
> **GNU GPL**. If you plan on integrating OpenBLT into your **closed source project**,
> a **commercial license** can be obtained.

- 若你的项目开源（GPL 兼容）→ 可自由使用
- 若产品要闭源交付 → **必须购买商业授权**，否则整个产品都要以 GPL 开源

（注：网上有文章称 OpenBLT 是"BSD 风格宽松许可、可商用"，**这是错误的**，
以官网 README 的双重许可声明为准。）

### 2.3 MCUboot：Apache-2.0，宽松但架构不匹配

Apache-2.0，商用友好（保留声明即可）。可安全借鉴其设计思想与算法。
但如 1.2 所述，它的传输层实现无法低成本搬到裸机上。

### 2.4 结论：协议层自研

不是"不想复用"，是"没有可用的合规实现"：ST 的许可锁死在 ST 芯片，
OpenBLT 的 GPL 会传染，MCUboot 的架构搬不动，MIT 那份只有发送端。

**但"自研"不等于"从零发明"**：YMODEM 是 Chuck Forsberg 公开的规范，
按规范实现不侵权；阅读 ST 的实现来理解协议细节也不侵权（**复制代码才侵权**）。

**顺带说明：分层架构天然规避了许可风险。**
`target/` 层本来就不跨平台复用（各芯片寄存器、页大小、编程粒度都不同），
STM32 版用 HAL 完全合规（SLA0048 允许在 ST 芯片上使用）；
而需要跨平台共用的 `core/` 层，恰好是我们的自研代码。

| 层 | 跨平台复用？ | 代码来源 | 许可风险 |
|---|---|---|---|
| `core/` | ✅ 要复用 | 自研（CRC、meta 已完成） | 无 |
| `port/` | ✅ 要复用 | 自研接口 | 无 |
| `target/stm32f4/` | ❌ 平台专属 | HAL 库 | 无 |
| `target/gd32f30x/` | ❌ 平台专属 | GD32 官方库 | 无 |
| `target/ch32*/` | ❌ 平台专属 | CH32 官方库 | 无 |

---

## 三、值得借鉴的设计（思想可自由使用）

调研这两个项目的最大收获其实是**设计思想**，这部分不受版权保护，可以放心采用。

### 3.1 采自 OpenBLT

| 设计 | 说明 | 本项目是否采用 |
|---|---|---|
| **Backdoor 入口** | 复位后固定时间窗内监听特定触发，强制进入 Bootloader | ✅ 已采用（300ms 窗口） |
| **协议与传输层解耦** | 同一套协议可跑在不同物理层上 | ✅ 已采用（core / port 分层） |
| **启动时校验合法性** | 每次上电校验用户程序，无效则留在 Bootloader | ✅ 已采用（状态机 + 可选 CRC32） |
| **伪向量表 + 中断重路由** | Bootloader 掌握主向量表，跳转后重路由到用户向量表 | ❌ 不采用——APP 直接设 `SCB->VTOR` 更简单，现代 Cortex-M3/M4 原生支持 |
| **S-record 格式** | 固件自带地址信息 | ⏸ 暂不采用——先用裸 bin + 固定地址，后期可扩展 hex/srec 解析 |
| **Seed/Key 鉴权** | 防止未授权刷写 | ⏸ 预留钩子，暂不实现 |
| **Checksum 写入** | 刷写完成后写入校验值 | ✅ 已采用（配置区记录 CRC32） |

### 3.2 采自 MCUboot

| 设计 | 说明 | 本项目是否采用 |
|---|---|---|
| **`image_ok` 确认语义** | APP 自检通过后才"转正"新固件，否则下次复位回滚 | ✅ 已采用（`TESTING` → `VALID`） |
| **trial boot 自动回滚** | 新固件试运行，失败自动退回旧固件 | ✅ 已采用（启动计数，连续 3 次未确认则作废） |
| **image trailer 元数据** | 镜像尾部存放状态与校验信息 | ✅ 变体采用——放在独立配置扇区（因 Flash 受限，无 A/B 空间） |
| **A/B 双槽 + swap** | 最彻底的防变砖方案 | ❌ 装不下（需 2×880KB，本芯片仅 1MB） |

### 3.3 我们相对两者的额外设计

- **配置区日志式槽位轮转**：128KB 切成 2048 个 64B 槽，状态变更只追加写 64 字节。
  OpenBLT 与 MCUboot 都没有针对"每次上电记启动计数"这一场景做优化，
  直接写会导致每次上电都擦扇区（1 秒延迟 + 寿命消耗）。
- **原子提交的显式顺序约束**：先置 `DOWNLOAD` 再擦 APP 区，写进 README 作为强制约束。
- **编译期分区自检**：改错地址直接编译不过。

---

## 四、项目实施清单

### P0 · 数据层【已完成】

`config/bl_config.h`、`core/bl_types.h`、`core/bl_crc.c/.h`、`core/bl_meta.c/.h`、`port/bl_port.h`

### P1 · 协议层

| 项 | 文件 | 要点 | 验收标准 |
|---|---|---|---|
| YMODEM 接收状态机 | `core/bl_ymodem.c/.h` | 按规范自研；SOH/STX 双模、CRC16、NAK 重传、CA 中止、首包解析文件名+大小 | 用 SecureCRT/Xshell 发送 1MB bin 能收全 |
| 镜像校验 | `core/bl_verify.c/.h` | 向量表合法性（SP 落在 SRAM 范围、入口为奇数 Thumb 地址）+ 整体 CRC32 比对 | 空片 / 半写片必须判为无效 |

### P2 · 流程层

| 项 | 文件 | 要点 |
|---|---|---|
| 升级会话编排 | `core/bl_session.c/.h` | **先置 DOWNLOAD → 擦除 → 接收 → 校验 → 提交 TESTING** 的原子顺序；擦除在收首包拿到 size 后一次性完成 |
| 启动决策 | `core/bl_boot.c/.h` | 读 meta → Backdoor 窗口 → 状态判定 → 启动计数 → 跳转或进 IAP |

### P3 · 移植层

| 项 | 文件 | 要点 |
|---|---|---|
| 移植模板 | `port/bl_port_template.c` | 填空式模板，含每项约束的注释说明 |
| STM32F4 Flash | `target/stm32f4/bl_port_flash_stm32f4.c` | 不等长扇区表、S4=64KB 特殊处理、32 位字编程、非对齐补齐 |
| STM32F4 UART | `target/stm32f4/bl_port_uart_stm32f4.c` | 115200 8N1，超时读 |
| STM32F4 系统 | `target/stm32f4/bl_port_system_stm32f4.c` | 跳转八步、IWDG、毫秒时基 |

### P4 · 应用层

| 项 | 文件 | 要点 |
|---|---|---|
| 主流程 | `app/bl_main.c` | 初始化 → CRC 自检 → meta 初始化 → 启动决策 → IAP 循环 |
| Keil 挂载 | `MDK-ARM/*.uvprojx` | `Bootloader/` 加入编译；IROM1 改 `0x08000000 / 0x4000` |

### P5 · 串口与波特率

| 项 | 说明 |
|---|---|
| 固定 115200 | IAP 阶段默认波特率 |
| **波特率修改接口** | 独立接口 `bl_port_uart_set_baudrate(uint32_t baud)`，供将来实现"命令切换波特率提速" |

> 用户决定：当前固定 115200，但**预留接口**，后续可做"握手后切换到 921600"提速。

### P6 · APP 侧配套（暂缓）

| 项 | 说明 |
|---|---|
| `LUMOS-app` 工程 | 从 LUMOS-Link 拷一份，IROM1 改 `0x08004000 / 0xDC000` |
| `SCB->VTOR` 重定位 | APP 的 `main()` 最开始设置向量表偏移 |
| 确认钩子 | APP 自检通过后调用确认接口把固件转正 |
| 请求升级钩子 | APP 收到升级命令 → 写 `UPDATE_REQ` → 软复位 |

> 用户决定：**暂不做**，等要联调测试时再建。

### P7 · 上位机

| 方案 | 说明 |
|---|---|
| 零成本（当前采用） | 直接用 SecureCRT / Xshell / MobaXterm / Tera Term 的 YMODEM 发送功能 |
| 自研（后期可选） | Python + pyserial，做成一键升级工具，支持进度条与批量 |

### P8 · 验证

| 阶段 | 内容 |
|---|---|
| 静态 | Keil `-O2` 编译通过；查 `.map` 确认 **ROM < 16KB**（硬约束） |
| 宿主机 | Python 脚本按规范生成测试帧，确认 CRC16 参数与工具一致 |
| VM 交叉编译 | 在 Ubuntu 编译机验证 |
| 板端 | DAPLink 烧 Bootloader，串口实测 YMODEM 升级 |
| **异常注入** | 升级中途断电（3 个不同时点）→ 每次都必须能恢复 |
| **回滚验证** | 故意刷一个会 HardFault 的 APP → 连续 3 次后应自动退回 IAP |

### P9 · 未来移植（GD32 / CH32）

| 项 | 说明 |
|---|---|
| 新增 `target/gd32f30x/` | 扇区为均匀 2KB/页，与 F4 的不等长扇区不同，需重写扇区表 |
| 新增 `target/ch32*/` | CH32 多为 RISC-V（如 CH32V307），跳转流程与 Cortex-M 有差异，需核对 |
| 改动范围 | 只改 `config/bl_config.h` 的三个地址 + 新增 target 目录，`core/` 一行不动 |

---

## 五、已确认的决策

| # | 事项 | 决定 |
|---|---|---|
| 1 | 升级协议 | **YMODEM-1K**（对比 XCP / SMP 后确定，理由见第一章） |
| 2 | 协议实现方式 | **自研**（合规 + 跨平台 + 实现量最小） |
| 3 | Bootloader 区大小 | **16KB**（`0x08000000`，扇区 S0） |
| 4 | 配置区方案 | **独立配置扇区**（`0x080E0000`，S11，方案二） |
| 5 | 仓库 | `ZhangKeLiang0627/LUMOS-bootloader`，**private** |
| 6 | IAP 波特率 | **固定 115200**，但预留修改接口 |
| 7 | 固件校验 | **CRC32**，暂不加签名（预留钩子） |
| 8 | APP 侧工程 | **暂缓**，联调时再建 |

---

## 六、ROM 占用预算（16KB 硬约束）

| 模块 | 预估 | 说明 |
|---|---|---|
| YMODEM 状态机 | 2-3 KB | 自研精简实现 |
| CRC16 + CRC32 | 0.5 KB | 含 nibble 表 |
| meta + verify + session + boot | 1.5-2 KB | |
| 跳转 / 看门狗 / 时基 | 1 KB | |
| UART 驱动 | 1-2 KB | 视中断/DMA 方案 |
| Flash 驱动 | 1-2 KB | |
| 调试输出（可选） | 1-2 KB | 若用 `printf` 家族，实测常超预期 |
| **小计** | **8-13 KB** | |
| 余量 | 3-8 KB | |

**风险点**：`BL_DEBUG_LOG` 打开时若链接了完整 `printf`（含浮点），可能一次吃掉 3-4KB。
必要时改用精简 `bl_printf`（只支持 `%d / %x / %s / %c`）。
编译后必须查 `.map` 确认，这是 P8 的硬性验收项。
