# 移植指南

> 这份文档回答一个问题：**怎么把 `Bootloader/` 这个文件夹搬到另一颗芯片的工程里，并且尽快跑起来。**

---

## 一、设计目标：它是一份"可搬走的库"，不是一个"适配多芯片的工程"

先说清楚不做什么：

- ❌ 不追求"一个工程里同时支持 F401/F405/GD32"
- ✅ 追求"同一份 `Bootloader/` 目录，原样拷到任何工程都能用"

这个区别决定了整个结构。如果目标是前者，做法会是"到处加 `#if
defined(STM32F401xE)`"；而目标是后者时，做法是**把芯片差异全部赶到少数几个
文件里，让其余部分对芯片一无所知**。

于是有了三层：

| 层 | 目录 | 知道的 | 换芯片时 |
|---|---|---|---|
| 应用层 | `app/` | 只调用 `core` 与 `port` | 不动 |
| 核心层 | `core/` | 只调用 `port/bl_port.hpp` 的 19 个函数 | **一行都不动** |
| 配置层 | `bl_config.h` + `presets/` | 只有数字：容量、地址、时钟、开关 | 选一个预置或填几个数 |
| 移植层 | `port/` + `target/` | 具体芯片的寄存器和厂商库 | **这里是唯一写代码的地方** |

`port/bl_port.hpp` 是唯一的分界线。`core/` 看不到 HAL、看不到寄存器、看不到
GD32 的固件库 —— 它只能看到"擦除这个地址范围""从这个串口读 N 个字节"。

---

## 二、移植三步

### 第 1 步：把目录拷进目标工程

把整个 `Bootloader/` 拷过去（放在哪一层无所谓）。它内部所有 `#include` 都是
相对库根目录的：

```cpp
#include "bl_config.h"           // 库根
#include "core/bl_types.hpp"     // 子目录带前缀
#include "port/bl_port.hpp"
#include "target/stm32f4/bl_target_config.h"
```

所以编译器**只需要一个库的 include 路径**：

```
-I <工程>/Bootloader
```

### 第 2 步：选目标芯片

打开 `bl_config.h` 第一节，三种方式任选：

**A. 什么都不做（推荐）** —— 如果你已经为 HAL 定义了 CMSIS 器件宏
（Keil 里 Options → C/C++ → Define 的 `STM32F405xx` / `STM32F401xE`），
`bl_config.h` 会认出来并自动挑预置，**不需要新增任何宏**。

**B. 显式指定** —— 取消 `bl_config.h` 里某一行注释：

```c
#include "presets/stm32f405xx.h"
```

**C. 没有预置** —— 照 `presets/stm32f405xx.h` 的格式自己写一个，或直接在
`bl_config.h` 的「手填区」把 `BL_FLASH_SIZE` / `BL_SRAM_END` / 时钟参数填上。
填漏了会编译报错，不会带着错值上板。

> **时钟参数必须和 `stm32f4xx_hal_conf.h` 里的 `HSE_VALUE` 一致**，
> 否则 HAL 会按错误的基准反算系统频率，串口波特率全错（现象是满屏乱码）。
> 库里有编译期自检，不一致直接编译不过：
> 在工程选项里加 `HSE_VALUE=25000000` 即可（这也是 Keil 里唯一可能要新增的宏）。

### 第 3 步：加编译 + 接入口

**要加入编译的文件**（17 个，都在库里）：

```
core/bl_log.cpp  bl_crc.cpp  bl_meta.cpp  bl_ymodem.cpp
     bl_verify.cpp  bl_session.cpp  bl_boot.cpp
app/bl_entry.cpp
target/<平台>/*.cpp          ← stm32f4 有 4 个
```

**不要加入编译**：`port/bl_port_template.cpp`（它是模板，会和 target 实现
重复符号）。

**入口二选一**：

| 方式 | 做法 | 适合 |
|---|---|---|
| 库自带 `main()`（默认） | 什么都不用做；把宿主工程自带的 `main.c` 移出编译 | Bootloader 是一份独立固件 |
| 接到已有 `main()` | 把 `bl_config.h` 里的 `BL_PROVIDE_MAIN` 设为 `0`，在你的 `main()` 里调 `bl::bl_entry()` | 想保留自己的启动代码 |

`bl_entry()` 内部会自己做平台初始化（时钟、时基、串口引脚），而且**是幂等的**
—— 你先初始化过一遍也不会冲突。

**宿主工程需要提供的（芯片支持包）**：
- 厂商 HAL / 固件库（STM32F4 就是 `STM32F4xx_HAL_Driver` + `CMSIS`）
- 启动文件 `startup_<器件>.s`
- CMSIS 的 `system_stm32f4xx.c`（提供 `SystemInit` / `SystemCoreClockUpdate`）
- `stm32f4xx_hal_conf.h`

**宿主工程需要移出编译的**（避免与库重复符号）：
- 自带的 `main.c`、`stm32f4xx_it.c`（库自带 SysTick 与异常处理）
- `stm32f4xx_hal_msp.c`（库自带 `HAL_MspInit`）

### 第 4 步：把 APP 的起始地址告诉 APP 侧

`bl_config.h` 会算出 `BL_APP_BASE`。APP 工程要：
1. Keil 的 IROM1 起始 = `BL_APP_BASE`，大小 = `BL_APP_SIZE`
2. `main()` 开头设 `SCB->VTOR = BL_APP_BASE;`
3. 自检通过后调 `bl::app_confirm()`（见 `core/bl_meta.hpp`），
   否则固件会停在 TESTING，启动次数到上限后被当成"跑不起来"而回滚

---

## 三、两类移植，工作量差很多

### 情况一：同族芯片（复用已有 target）

F401 / F405 / F407 / F411 / F415 / F417 共用 `target/stm32f4/`：

- 扇区布局同一条规则（S0-S3 16KB / S4 64KB / S5+ 128KB），
  驱动按地址算扇区号，**不硬编码扇区表**
- Flash 编程单位都是 32 位字

所以**只改 `bl_config.h` 的几个数字**。实测：F401（512KB）与 F405（1MB）
用同一份源码，只是链接区域和预置不同，两者 ROM 占用仅差 24 字节。

### 情况二：异族芯片（写 target）

以 GD32F303 为例，差异在三个地方：

| 项 | STM32F4 | GD32F303 |
|---|---|---|
| 擦除单位 | 扇区，**不等长**（16/64/128KB） | 页，**等长**（512KB 型号每页 2KB） |
| 编程单位 | 32 位字 | 16 位半字 |
| 时钟树 | PLL 从 HSE 分频后倍频 | PLL 从 PREDV 输出倍频，切换前要降 AHB |
| 跳转 | Cortex-M4 | Cortex-M4（同） |

做法：复制 `port/bl_port_template.cpp` 到 `target/gd32f30x/`，按 19 个函数的
注释逐项填。`core/` 完全不动 —— 因为 core 只按"字节地址 + 字节长度"提要求，
它不知道擦除单位是扇区还是页。

`presets/gd32f303re.h` 里已经写好了 GD32 的容量/分区数据，并注明了那三处差异。

**CH32V307 之类 RISC-V 内核要注意**：向量表寄存器不是 `SCB->VTOR`，
跳转前的准备流程也与 Cortex-M 不同，必须查对应手册，不能照搬。

---

## 四、分层约束（这是"优雅"能维持下去的原因）

规矩很简单，但要守住：

1. **`core/` 不得包含任何芯片头文件**
   只允许 `bl_config.h`、`core/*`、`port/bl_port.hpp`、`<cstdint>` 这类。
   一旦有人为了图快在 core 里写了一句 `HAL_GPIO_WritePin`，这层就废了。

2. **`core/` 不得假设 Flash 只能按扇区擦**
   只能调 `flash_erase(addr, len)`、`flash_sector_size(addr)`：
   扇区/页怎么划、编程粒度多大，是移植层的事。

3. **`app/` 不得直接调厂商库**
   需要延时就调 `bl::delay_ms()`，需要复位就调 `bl::system_reset()`。
   实测时就是靠这条约早发现了 `HAL_Delay` 混进 app 层。

4. **平台相关的可调项要么在 `bl_config.h`（芯片级），
   要么在 `target/<平台>/bl_target_config.h`（板级）**
   比如串口用哪一路、哪几个引脚 —— 它既不是芯片规格，也不通用，
   放在 target 的板级配置里最合适。

---

## 五、移植自查清单

上板前逐条过一遍：

- [ ] 编译期自检通过（分区不重叠、配置区基址对齐、HSE_VALUE 一致）
- [ ] 链接后 ROM 装得进 `BL_BOOT_SIZE`（用 `tools/build.py` 量）
- [ ] 上电看到 banner，且打印的 boot/app/meta 地址符合预期
- [ ] 打印的 `sysclk` 与 `bl_config.h` 的期望值一致（不一致就是时钟没配对）
- [ ] 分区布局检查没有报警（`[cfg] ... not on a sector boundary`）
- [ ] 空片状态能停在 `waiting for YMODEM transfer...` 而不是乱跳
- [ ] 刷一次固件能成功，且复位后能跳进 APP
- [ ] 拔电中断重来一次，仍能进 IAP

---

## 六、几个真踩过的坑

**1. `HSE_VALUE` 与 `BL_HSE_HZ` 不一致**
现象：串口全是乱码，但程序"看起来在跑"。原因：HAL 用 `HSE_VALUE` 反算
`SystemCoreClock`，进而算 UART 分频，差了 3 倍波特率就完全对不上。
库里加了编译期 `#error` 挡住这种情况。

**2. 配置区基址没落在扇区边界**
现象：平时没事，一升级就把 APP 头部擦掉。`bl_entry()` 上电会检查并拒绝
进入 IAP（宁可停留在报错，也不做可能毁数据的擦除）。

**3. 看门狗比单次阻塞还短**
现象：设备周期性重启，日志重复出现。原因：`HAL_UART_Receive` 一口气阻塞
3 秒、整扇区擦除最坏 4 秒，而看门狗只有 2 秒。
对策：串口等待切片成 50ms 并逐片喂狗；看门狗超时按最坏擦除时间取 6 秒
（`BL_WATCHDOG_TIMEOUT_MS`）。

**4. 栈太小**
ST 的 CubeMX 启动文件默认给 16KB 栈 + 8KB 堆。Bootloader 用不到这么多，
而且我们禁用了动态分配，所以裁剪到 4KB / 0。**但要留意**：`Session` 对象
带着 1KB 的 YMODEM 缓冲，是栈上的局部变量，栈不能小于 2KB。

**5. 用标准 `printf` 打印日志**
现象：ROM 直接超 16KB，链接报 `L6406E`。原因：链接 `vsnprintf` 会连带
`_printf_wctomb` / `btod` / `bigflt0`（浮点格式化）一起吃进约 6.5KB。
对策：库里自带 `core/bl_log`，只支持实际用到的格式符，开销约 2KB。
