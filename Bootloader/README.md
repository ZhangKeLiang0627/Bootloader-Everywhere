# Bootloader —— 可搬走的串口 IAP 引导程序

一个与芯片无关的串口 IAP Bootloader。**整个 `Bootloader/` 目录可以原样拷进
任何 Cortex-M 工程**：改一个配置文件、加一份平台适配，就能跑起来。
升级协议用 **YMODEM-1K**（公共协议，SecureCRT / Xshell / MobaXterm /
Tera Term / lrzsz 原生支持，PC 端不需要专门写上位机）。

已随库提供的平台适配：**STM32F4 全系**（F401 / F405 / F407 / F411 / F415 / F417）。
已实测通过编译与链接：F401RE（512KB）、F405RG / F407（1MB）。

---

## 一、它是什么，不是什么

- ✅ **一份可搬走的库**：`Bootloader/` 是一个自包含目录，内部引用只用相对库根的
  路径，所以宿主工程只需要一个 include 路径。
- ❌ **不是一个"在一个工程里同时支持多颗芯片"的工程**：那种做法需要在核心
  代码里到处 `#if defined(...)`，恰恰会毁掉可移植性。

区别在于把差异放在哪：

```
        ┌────────────────────────────────────────────┐
        │  app/       入口：bl_entry()               │  不动
        ├────────────────────────────────────────────┤
        │  core/      状态机 / YMODEM / 校验 / 决策   │  一行都不动
        ├────────────────────────────────────────────┤
        │  bl_config.h   只有数字：容量、地址、时钟    │  改几个数
        │  presets/      现成的芯片参数               │
        ├────────────────────────────────────────────┤
        │  port/bl_port.hpp   19 个函数的契约          │  契约
        ├────────────────────────────────────────────┤
        │  target/<平台>/     唯一写代码的地方         │  写这里
        └────────────────────────────────────────────┘
```

`core/` 看不到 HAL、看不到寄存器、看不到 GD32 的固件库 —— 它只知道
"把这段字节写到那个地址""从这个串口读 N 个字节"。所以搬家的代价与
芯片厂商无关，只与"要写几个移植函数"有关。

**完整移植说明见 [`docs/PORTING.md`](docs/PORTING.md)。**

---

## 二、目录结构

```
Bootloader/
├── README.md                  本文档
├── bl.hpp                     总头文件（宿主工程只需认识它）
├── bl_config.h                ★ 移植时唯一要改的文件
├── presets/                   现成芯片参数：选一个 include 即可
│   ├── stm32f401xe.h          512KB，HSE 25MHz → 84MHz
│   ├── stm32f405xx.h          1MB，HSE 8MHz → 168MHz
│   ├── stm32f407xx.h          1MB
│   └── gd32f303re.h           非 ST 平台示例（含差异说明）
├── core/                      平台无关（移植时不动）
│   ├── bl_types.hpp           Status / FwState / IapResult
│   ├── bl_log.hpp/.cpp        轻量格式化输出（比 vsnprintf 省 4.5KB）
│   ├── bl_crc.hpp/.cpp        Crc16（YMODEM 帧）与 Crc32（整镜像）
│   ├── bl_meta.hpp/.cpp       配置区：固件状态机、日志式槽位
│   ├── bl_verify.hpp/.cpp     镜像合法性：向量表 + CRC32
│   ├── bl_ymodem.hpp/.cpp     YMODEM-1K 接收端
│   ├── bl_session.hpp/.cpp    升级会话：原子提交编排
│   └── bl_boot.hpp/.cpp       上电启动决策
├── port/
│   ├── bl_port.hpp            ★ 移植契约（19 个函数）
│   └── bl_port_template.cpp   填空模板（勿加入编译）
├── target/stm32f4/            现成适配：STM32F4 全系
│   ├── bl_platform_stm32f4.cpp    时钟树 / 时基 / SysTick / 异常兜底
│   ├── bl_port_flash_stm32f4.cpp  扇区规则自适应，不硬编码扇区表
│   ├── bl_port_uart_stm32f4.cpp   自持句柄与引脚，不依赖宿主 usart.c
│   ├── bl_port_system_stm32f4.cpp 跳转 / 看门狗 / 复位
│   ├── bl_target_config.h         板级差异（串口用哪一路、哪几个脚）
│   └── bl_target_internal.hpp     适配层内部共享
├── app/
│   ├── bl_entry.hpp           对外唯一动作接口
│   └── bl_entry.cpp           主流程 + 可选的自带 main()
├── docs/
│   ├── PORTING.md             ★ 移植指南
│   ├── TEST_PLAN.md           以"不变砖"为核心的测试方案
│   └── REUSE_AND_PLAN.md      开源复用调研与许可分析
└── tools/
    ├── build.py               命令行编译 / ROM 测量 / 导出 bin、hex
    ├── board.py               DAPLink 板端工具：备份 / 擦除 / 烧写 / 读回 / 串口
    └── flash_blob/            RAM 小程序烧写（应对探针不支持的场景）
```

---

## 三、快速上手

### 1. 移植（三步，详见 `docs/PORTING.md`）

1. 把 `Bootloader/` 拷进目标工程，加一个 include 路径 `-I .../Bootloader`
2. 改 `bl_config.h` 第一节：选一个预置（或手填容量与时钟）
3. 把 `core/*.cpp` + `app/bl_entry.cpp` + `target/<平台>/*.cpp` 加入编译

宿主工程需要**移出编译**：自带 `main.c`、`stm32f4xx_it.c`、
`stm32f4xx_hal_msp.c`（库自带这三者提供的功能，否则重复符号）。
`port/bl_port_template.cpp` 也不要加进来。

### 2. 命令行编译与量体积（不用打开 Keil）

```bash
python tools/build.py                     # 默认 stm32f401xe，-Oz
python tools/build.py -c stm32f405xx      # 换芯片
python tools/build.py --bin build/bl.bin  # 导出可直接烧写的 bin
python tools/build.py --all               # 三颗芯片 × 各优化等级对比
```

### 3. 板端操作

```bash
python tools/board.py info                # 芯片信息 / 扇区占用 / 配置区状态
python tools/board.py backup              # 整片备份到桌面
python tools/board.py flash build/bl_f401.bin
python tools/board.py monitor --seconds 10
```

---

## 四、防变砖设计（第一优先级需求）

### 1. Bootloader 自身不可被覆盖
Bootloader 常驻 S0（16KB），升级只操作 APP 区与配置区，**永远不擦写自己**。
Flash 驱动里还加了一道硬保护：`flash_erase()` 收到落在 Bootloader 区的地址
直接拒绝。就算上层逻辑写出 bug，重刷入口也还在。

### 2. 原子提交（顺序就是安全本身）

```
① 配置区置 fw_state = DOWNLOAD      ← 必须先在"不可信"状态下动手
② 擦除 APP 区所需扇区
③ 接收 YMODEM 数据并写入
④ 算整镜像 CRC32
⑤ 校验通过 → fw_state = TESTING，记录 size / crc32 / 版本
⑥ 复位，重新走启动决策
```

第①步必须在第②步之前。顺序写反（先擦 APP 再置标志）时，擦除中途掉电会留下
"状态 VALID 但 APP 已是空片"的**必砖组合**。

按上述顺序，任意时刻掉电的结果都是安全的：

| 掉电时刻 | 配置区状态 | 上电行为 |
|---|---|---|
| 置 DOWNLOAD 之前 | `INVALID` / `VALID` | 正常（原固件未动） |
| 置 DOWNLOAD 之后 | `DOWNLOAD` | 停在 IAP，可重刷 |
| 擦除 APP 中途 | `DOWNLOAD` | 停在 IAP，可重刷 |
| 传输数据中途 | `DOWNLOAD` | 停在 IAP，可重刷 |
| CRC 校验失败 | `DOWNLOAD` | 停在 IAP，可重刷 |

### 3. 试运行与自动回滚（挡住"能过校验但跑不起来"）
CRC 正确不代表逻辑正确（可能一初始化就 HardFault）。所以新固件先置于 `TESTING`：

- 每次启动把 `boot_attempts` 加一并写回配置区，再跳转 APP；
- APP 自检通过后调 `bl::app_confirm()` 置 `VALID` 并清零计数；
- 连续 `BL_BOOT_MAX_ATTEMPTS`（默认 3）次没等到确认 → 判为有运行时缺陷，
  作废并停在 IAP。

### 4. 看门狗
默认开启 IWDG，超时 `BL_WATCHDOG_TIMEOUT_MS`（默认 6 秒）。取值依据是
**"中间没机会喂狗的最长阻塞"**：一次整扇区擦除最坏 4 秒。
串口等待已切成 50ms 小片并逐片喂狗，所以不受长超时影响。

有三点必须知道：

- **IWDG 一旦启动就无法停止**，只能靠复位；
- **系统复位不会复位它**（IWDG 在 VDD 域）。也就是说 Bootloader 开了它之后，
  APP 里它仍继续跑着 —— 所以 **APP 必须持续喂狗**。这既是约束也是好处：
  APP 真跑飞了，看门狗会把它拉回来，启动计数随之增加，最终触发上一节的自动回滚；
- 初始化顺序必须是**先 `0xCCCC` 启动、再改 PR/RLR**。反过来的话，LSI 尚未起振、
  IWDG 没有时钟驱动，`SR` 的 `PVU/RVU` 会一直保持 1，
  任何"等它清零"的循环都会死锁（表现为 APP 一进去就再也不出声）。

### 5. Backdoor 强制进入
上电后 `BL_BACKDOOR_WINDOW_MS`（默认 300ms）窗口内收到 `BL_BACKDOOR_CHAR`
（默认 `0x7F`，串口工具里好按）即强制进 IAP，忽略固件状态。
用于 APP 正常但需要强制升级的场景，也是开发期的救命通道。

### 6. 分区布局上电自检
`bl_entry()` 会检查 BOOT / APP / META 三个基址是否正好落在扇区边界上。
**不通过就绝不进 IAP** —— 擦除是按分区地址算的，地址错了会毁掉邻近区域。
若此时固件本身可启动，仍放它跑（读操作永远安全）。

### 7. 跳转前必须把内核状态清干净（一个非常隐蔽的坑）

`jump_to_app()` 里有一步的顺序特别容易写错：
**「关 SysTick」必须放在 `HAL_RCC_DeInit()` 之后。**

原因是 `HAL_RCC_DeInit()` 内部末尾会调用 `HAL_InitTick()`，
也就是**重新把 SysTick 配成 1ms 并使能其中断**。按直觉把它写在前面，
就会被它悄无声息地重新打开。后果是：

- APP 一跑起来就不断被 SysTick 中断打断；
- APP 的向量表里 `SysTick_Handler` 通常是空的（`Default_Handler` = 一条 `B .`）；
- 于是 APP 直接卡死在异常处理里，**表现为完全不输出任何字符**。

光看串口根本无法定位，必须用调试器读 `SCB->ICSR` 才能看到 `VECTACTIVE = 15`。
另外 `SysTick` 与 `PendSV` 属于**系统异常、不在 NVIC 里**，
那把"清 NVIC"的循环管不到它们，必须单独写 `PENDSTCLR` / `PENDSVCLR` 清挂起位。

APP 侧还有三件事要自己做对（重定位 VTOR、使能 FPU、清 SysTick），
完整清单与真板实测记录见 `TestApp/README.md`。

---

## 五、配置区（Flash 参数区）

采用**日志式槽位轮转**：

- 配置扇区（默认 128KB）切成 2048 个 64 字节槽位；
- 每次状态变更**顺序追加**写一个新槽，不擦除；
- 槽内带自增序号与 CRC32，读取时扫描取序号最大的有效槽；
- 写满整个扇区后才擦除一次。

为什么不能原地擦写：一次状态写入只有 64 字节、耗时几十微秒；而擦一个 128KB
扇区要 1 秒左右并消耗一次寿命。每次上电记启动计数都这么干，既卡顿又短命。

---

## 六、YMODEM 交互流程

```
Bootloader                     PC 上位机
    |                               |
    |  <------ 'C' (0x43) ----------|  请求 CRC 模式
    |                               |
    |  ---- SOH 0 帧（文件名+大小）->|  首包
    |  [按 size 一次性擦除所需扇区]
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

**擦除时机**：收到首包拿到 size 后**一次性擦完**再 ACK。这样擦除耗时落在
PC 等 ACK 的窗口里，不会触发上位机超时重传。边收边擦会撑爆多数上位机的默认超时。

**CRC32 由板子自己算**，不依赖上位机下发：

- YMODEM 的帧级 CRC16 + ACK/NAK 已经保证了"PC → 板子"的传输正确性，这是协议本职；
- 整镜像 CRC32 的职责是**防 Flash 位翻转 / 擦写不完整**，属于本端自检；
- 于是不需要约定"上位机用什么口径算 CRC"，
  **标准 YMODEM 工具（SecureCRT 等）无需改造就能用**；
- 也彻底避开"末包填充字节是否计入 CRC"的陷阱（不同工具填充值可能是
  `0x00` / `0x1A` / `0xFF`，详见 `bl_verify.cpp` 注释）。

---

## 七、实测 ROM 占用（16KB 硬约束）

用 `tools/build.py` 真实编译 + 链接测出来的数据（AC6）：

| 芯片 | 配置 | RO 占用 | 16KB 余量 |
|---|---|---|---|
| F401RE | `-O2` | — | ✘ 链接失败（超出） |
| F401RE | `-Os` + 日志 | 16116 B (15.74 KB) | 268 B（太紧） |
| F401RE | **`-Oz` + 日志** | **14596 B (14.25 KB)** | **1788 B ✔ 推荐** |
| F401RE | `-Os` 无日志 | 11928 B (11.65 KB) | 4456 B（发布版） |
| F405RG | `-Oz` + 日志 | 14560 B (14.22 KB) | 1824 B ✔ |
| F407 | `-Oz` + 日志 | 14560 B (14.22 KB) | 1824 B ✔ |

三条关键结论：

1. **优化等级影响极大**：`-O2` 比 `-Os` 多占约 4KB。AC6 下请用 `-Os` / `-Oz`。
2. **不要链接标准 `printf` 家族**：`vsnprintf` 会连带 `_printf_wctomb` / `btod` /
   `bigflt0`（浮点格式化）一起吃进约 **6.5KB**。库自带 `core/bl_log`，
   开销约 2KB，净省 4.5KB。
3. 需要的 HAL 模块只有 12 个，**tim / exti 等不要加进编译**。

RAM 侧：静态数据仅约 430 字节；启动文件的栈按 4KB、堆按 0 裁剪
（`Session` 对象带着 1KB 的 YMODEM 缓冲，是栈上局部变量，栈不能小于 2KB）。

---

## 八、编译环境

| 项 | 值 |
|---|---|
| 编译器 | ARM Compiler 6（AC6） |
| C++ 标准 | C++14 |
| RTTI | 关闭 |
| 异常 | 关闭（`-fno-exceptions`） |
| 优化 | `-Oz`（开发）/ `-Os` 且关日志（发布） |

代码风格：`enum class`、`constexpr`、`static_assert`；
**不使用**异常、动态内存、RTTI、STL 容器。

### Keil 工程要点

1. 加入编译：`core/*.cpp`、`app/bl_entry.cpp`、`target/stm32f4/*.cpp`
   （**不含** `port/bl_port_template.cpp`）
2. include 路径只加一个：`..\Bootloader`
3. IROM1 = `0x08000000` / `0x4000`
4. C/C++ → No Exceptions；优化选 `-Oz`（没有该选项就填进 Misc Controls）
5. 移出编译：`Core/Src/main.c`、`gpio.c`、`usart.c`、`stm32f4xx_it.c`、
   `stm32f4xx_hal_msp.c`（保留 `system_stm32f4xx.c`）
6. F401 这类 HSE 不是 8MHz 的板子，加宏 `HSE_VALUE=25000000`
7. 编译后查 `.map` 或用 `tools/build.py` 确认 ROM < 16KB

---

## 九、关于串口 DMA：当前不用，接口已预留

**当前阶段（115200、YMODEM 停等传输）不需要 DMA**：

- YMODEM 是停等协议，收 1KB 约 89ms，之后有整段时间处理，不存在"边收边处理"压力；
- 轮询不会丢字节：115200 下字节间隔约 87µs，单字节轮询开销只有几微秒；
- 擦除的 1-4 秒阻塞发生在首包 ACK 之前，此刻 PC 正在等 ACK、不会发数据；
- DMA 的收益在 bootloader 场景用不上，反而引入中断竞态与额外 ROM。

**什么时候必须上**：波特率提到 921600 时字节间隔降到约 10.8µs，轮询就濒临丢字节。
届时把 `uart_read` 换成 DMA + 环形缓冲即可 —— `bl_port.hpp` 一行不改。
`uart_set_baudrate()` 也已预留，用于实现"握手 115200、传输切 921600"。

---

## 十、参考资料

| 项目 | 借鉴了什么 |
|---|---|
| **OpenBLT**（Feaser） | 分层解耦（应用配置 / 核心逻辑 / 硬件抽象 / 通信）、Backdoor 入口、启动时校验固件 |
| **MCUboot** | `image_ok` 确认语义、trial boot 与自动回滚、原子提交的元数据设计 |
| **ST AN2606 / AN4657** | IAP 跳转前必须做的准备（关中断、清挂起、设 MSP、`__set_CONTROL(0)`） |

**没有直接复用它们的代码**，原因是许可：ST CubeF4 的 IAP 例程是 SLA0048
（限定仅 ST 芯片运行，移植 GD32 违反条款）、OpenBLT 是 GPLv3（闭源集成需商业授权）。
架构思想不受版权保护，YMODEM 是公开规范 —— 依法自行实现才是可持续的路子。
详细分析见 [`docs/REUSE_AND_PLAN.md`](docs/REUSE_AND_PLAN.md)。

**为什么不用 A/B 双分区**：它需要两份完整 APP 空间，本芯片 1MB 装不下
（APP 区 880KB，双份需 1.76MB）。改用"原地覆盖 + 状态机 + 自动回滚"，
这是 Flash 受限场景的经典折中。
