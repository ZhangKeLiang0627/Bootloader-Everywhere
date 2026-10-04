# 可复用代码调研 与 项目实施清单

本文档回答两个问题：
1. 能不能直接复用开源代码，省掉自己写的部分？
2. 整个项目要做哪些事，怎么排？

---

## 一、可复用代码调研（含许可分析）

调研了四类候选源。**结论先说：协议层必须自研，理由不是技术而是许可。**

| 来源 | 组件 | 许可协议 | 能否直接复用 |
|---|---|---|---|
| ST CubeF4 IAP 例程 | `ymodem.c` / `ymodem.h` | **SLA0048** | ❌ **仅限 ST 芯片** |
| ST CubeF4 IAP 例程 | `flash_if.c` / `flash_if.h` | **SLA0048** | ❌ 同上 |
| ST CubeF4 IAP 例程 | `common.c` / `main.c` / `menu.c` | SLA0048 | ❌ 同上 |
| **OpenBLT** | 全部 | **GPLv3** | ⚠️ 传染性，闭源产品慎用 |
| **MCUboot** | 全部 | **Apache-2.0** | ✅ 可用，但为安全启动设计，很重 |
| `zonque/xymodem-mini` | 发送端 | **MIT** | ⚠️ 只有发送端，Bootloader 用不上 |

### 1.1 ST 官方 IAP 例程：技术上完美，许可上不能用

路径就在本机（用户已装 Cube 包）：

```
C:\Users\11846\STM32Cube\Repository\STM32Cube_FW_F4_V1.27.1\
  Projects\STM324xG_EVAL\Applications\IAP\IAP_Main\
    src\ymodem.c        18930 B   ← YMODEM 收发完整实现
    inc\ymodem.h         4093 B
    src\flash_if.c       8537 B   ← Flash 擦除/编程封装
    inc\flash_if.h       3949 B   ← 含 S0-S11 扇区地址宏
```

而且 `STM324xG_EVAL` 用的就是 **STM32F407**，与 F405 同系列同 Flash 布局，
`flash_if.h` 里连 S0-S11 的地址宏都写好了，`APPLICATION_ADDRESS` 也已经预留了扇区 0 给 IAP。

**技术上它是现成的，一个字符都不用改。但许可不允许：**

Cube 包根目录 `Package_license.md` 声明的是 **SLA0048**，其中第 4 条原文：

> This software package or any part thereof, including modifications and/or
> derivative works of this software package, **must be used and execute solely
> and exclusively on or in combination with** microcontroller or microprocessor
> device manufactured by or for STMicroelectronics

即：**只能在 ST 芯片上运行**。移植到 GD32 / CH32 违反该条款。
（`ymodem.h` 头部还印着更老的 MCD-ST Liberty V2，同样是 ST 平台限定。）

### 1.2 OpenBLT：架构值得学，代码不能抄

OpenBLT 分层做得很漂亮，是最贴近本项目需求的参考。但它是 **GPLv3**。

GPLv3 的传染性意味着：一旦把它的代码并入你的产品并对外分发，
整个产品（含 APP）都要以 GPLv3 开源。对个人项目或开源项目无所谓，
但如果将来产品要闭源交付，这就是个问题。

**可安全借鉴的**（思想不受版权保护）：
- 四层解耦：应用配置 / 核心逻辑 / 硬件抽象 / 通信
- Backdoor 入口机制（上电时间窗内监听触发字符）
- 5-20KB 的 ROM 占用控制思路

### 1.3 MCUboot：许可宽松，但架构不匹配

Apache-2.0，商用友好（保留声明即可）。但它解决的是**安全启动 + A/B 双区回滚**，
代码体量大、依赖多，且 A/B 方案需要两份 APP 空间（本芯片 1MB 装不下）。

**可安全借鉴的**：
- `image_ok` 确认语义（APP 自检通过后才把新固件转正）
- trial swap 与自动回滚的状态设计

### 1.4 结论：为什么必须自研协议层

不是"不想复用"，是"没有可用的合规实现"：

- ST 的 → 许可锁死在 ST 芯片
- OpenBLT 的 → GPLv3 会传染
- MIT 的那份 → 只有发送端

**但"自研"不等于"从零发明"**：YMODEM 是 Chuck Forsberg 在 1980 年代公开的规范，
按规范实现不涉及侵权；阅读 ST 的实现来理解协议细节也不侵权（**复制代码才侵权**）。
我们按规范写一份干净的实现，既合规又能跨三个平台共用。

**顺带说明：这恰恰印证了分层架构的价值。**
`target/` 层本来就不该跨平台复用（各芯片 Flash 寄存器、页大小、编程粒度都不同），
所以 SLA0048 的限制在这一层不构成问题——STM32 版用 HAL 完全合规。
而需要跨平台复用的 `core/` 层，恰好是我们的自研代码。

| 层 | 跨平台复用？ | 代码来源 | 许可风险 |
|---|---|---|---|
| `core/` | ✅ 要复用 | **自研**（CRC、meta 已完成） | 无 |
| `port/` | ✅ 要复用 | 自研接口 | 无 |
| `target/stm32f4/` | ❌ 平台专属 | HAL 库（SLA0048 允许在 ST 芯片用） | 无 |
| `target/gd32f30x/` | ❌ 平台专属 | 用 GD32 官方库 | 无 |
| `target/ch32/` | ❌ 平台专属 | 用 CH32 官方库 | 无 |

---

## 二、项目实施清单

### P0 · 数据层【已完成】

| 项 | 文件 | 状态 |
|---|---|---|
| 分区与开关配置 | `config/bl_config.h` | ✅ 含编译期分区自检 |
| 公共类型与状态机 | `core/bl_types.h` | ✅ |
| CRC16/XMODEM + CRC32 | `core/bl_crc.c/.h` | ✅ 已用标准向量验证 |
| 配置区日志式槽位 | `core/bl_meta.c/.h` | ✅ |
| 移植契约 | `port/bl_port.h` | ✅ |

### P1 · 协议层

| 项 | 文件 | 要点 | 验收标准 |
|---|---|---|---|
| YMODEM 接收状态机 | `core/bl_ymodem.c/.h` | 按规范自研；SOH/STX 双模、CRC16、NAK 重传、CA 中止、首包解析文件名+大小 | 用 SecureCRT/Xshell 发送 1MB bin 能收全 |
| 镜像校验 | `core/bl_verify.c/.h` | 向量表合法性（SP 落在 SRAM、入口为奇数 Thumb 地址）+ 整体 CRC32 比对 | 空片/半写片必须判为无效 |

### P2 · 流程层

| 项 | 文件 | 要点 |
|---|---|---|
| 升级会话编排 | `core/bl_session.c/.h` | **先置 DOWNLOAD → 擦除 → 接收 → 校验 → 提交 TESTING** 的原子顺序；擦除在收首包后一次性完成 |
| 启动决策 | `core/bl_boot.c/.h` | 读 meta → Backdoor 窗口 → 状态判定 → 启动计数 → 跳转或进 IAP |

### P3 · 移植层

| 项 | 文件 | 要点 |
|---|---|---|
| 移植模板 | `port/bl_port_template.c` | 填空式模板，含注释说明每项约束 |
| STM32F4 Flash | `target/stm32f4/bl_port_flash_stm32f4.c` | 不等长扇区表、S4=64KB 特殊处理、32 位字编程、非对齐补齐 |
| STM32F4 UART | `target/stm32f4/bl_port_uart_stm32f4.c` | 115200 8N1，中断+FIFO 或 DMA，超时读 |
| STM32F4 系统 | `target/stm32f4/bl_port_system_stm32f4.c` | 跳转八步、IWDG、毫秒时基 |

### P4 · 应用层

| 项 | 文件 | 要点 |
|---|---|---|
| 主流程 | `app/bl_main.c` | 初始化 → 自检 CRC 实现 → meta 初始化 → 启动决策 → IAP 循环 |
| Keil 工程挂载 | `MDK-ARM/*.uvprojx` | 把 `Bootloader/` 加入编译；IROM1 改 `0x08000000 / 0x4000`（16KB） |

### P5 · APP 侧配套

| 项 | 说明 |
|---|---|
| `LUMOS-app` 工程 | 从 LUMOS-Link 再拷一份，IROM1 改 `0x08004000 / 0xDC000` |
| `SCB->VTOR` 重定位 | APP 的 `main()` 最开始设置向量表偏移 |
| 确认钩子 | APP 自检通过后调用 `bl_meta_confirm_app()`（或写一个约定地址的标志） |
| 请求升级钩子 | APP 收到升级命令 → 写 `UPDATE_REQ` → 软复位 |

### P6 · 上位机（可选）

| 方案 | 说明 |
|---|---|
| 零成本 | 直接用 SecureCRT / Xshell / MobaXterm 的 YMODEM 发送。**推荐先用这个**，不需要写代码 |
| 自研 | Python + pyserial（`xmodem` 库支持 YMODEM）+ 进度条；适合后期做成一键升级工具 |

### P7 · 验证

| 阶段 | 内容 |
|---|---|
| 静态 | Keil `-O2` 编译通过；查 `.map` 确认 **ROM 占用 < 16KB**（关键约束） |
| 宿主机 | PC 端 Python 脚本按规范生成测试帧，校验 CRC16 参数一致 |
| VM 交叉编译 | 在 Ubuntu 编译机验证（`vm-ubuntu-run` 流程） |
| 板端 | T113 不适用（非 STM32）；用 DAPLink 烧 bootloader，串口实测升级 |
| 异常注入 | 升级中途断电（拔电 3 次不同时点）→ 每次都必须能恢复 |
| 回滚验证 | 故意刷一个会 HardFault 的 APP → 连续 3 次后应自动退回 IAP |

### P8 · 未来移植（GD32 / CH32）

| 项 | 说明 |
|---|---|
| 新增 `target/gd32f30x/` | 扇区为 2KB/页的均匀布局，与 F4 不同，需重写扇区表 |
| 新增 `target/ch32*/` | CH32 多为 RISC-V（如 CH32V307），跳转流程与 Cortex-M 有差异，需核对 |
| 只改 2 处 | `config/bl_config.h` 的三个地址 + 新增 target 目录，`core/` 不动 |

---

## 三、待决策事项

| # | 事项 | 选项 | 建议 |
|---|---|---|---|
| 1 | YMODEM 实现方式 | ① 自研（合规、跨平台） ② 用 ST 的（仅 STM32 版） | **①**，一份代码三平台通用 |
| 2 | 仓库可见性 | private / public | 已建 **private**（与 LUMOS-Link 一致），可随时改 |
| 3 | APP 侧工程 | 现在建 `LUMOS-app` / 以后再建 | 建议**现在建**，否则 Bootloader 没有合法 APP 可测 |
| 4 | IAP 阶段波特率 | 115200 固定 / 命令切换 | 先 115200；预留切换指令的口子 |
| 5 | 是否加密/签名 | 不加密 / 加 CRC32 / 加签名 | 先用 CRC32；签名可在 `bl_verify` 预留钩子 |

---

## 四、ROM 占用预算（16KB 硬约束）

| 模块 | 预估 | 说明 |
|---|---|---|
| YMODEM 状态机 | 2-3 KB | 自研精简实现 |
| CRC16 + CRC32 | 0.5 KB | 含 nibble 表 |
| meta + verify + session + boot | 1.5-2 KB | |
| 跳转 / 看门狗 / 时基 | 1 KB | |
| UART 驱动 | 1-2 KB | 视中断/DMA 方案 |
| Flash 驱动 | 1-2 KB | |
| 调试输出（可选） | 1-2 KB | 若用 `printf` 家族，**实测常超预期** |
| **小计** | **8-13 KB** | |
| 余量 | 3-8 KB | |

**风险点**：`BL_DEBUG_LOG` 打开时若链接了完整 `printf`（含浮点），
可能一次吃掉 3-4KB。必要时改用精简 `bl_printf`（只支持 `%d/%x/%s/%c`）。
编译后必须查 `.map` 确认，这是 P7 的硬性验收项。
