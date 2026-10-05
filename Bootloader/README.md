# LUMOS-bootloader

串口 IAP Bootloader。YMODEM-1K 刷固件，上电零等待跳 APP，APP 运行中可被唤回。
换芯片只改一个文件（`bl_port_stm32f4.cpp`），`bl.cpp` 一行不用动。

**不做什么**：不做 A/B 双分区、不做自动回滚、不用看门狗。IAP 是人站在板子前刷的，
不是 OTA —— 这些机制的成本大于收益。

---

## 库就是这几个文件

```
Bootloader/
├── bl.h                 唯一入口：blRun() / blRequestUpdate() / 公共类型
├── bl.cpp               全部实现（日志/CRC/元数据/校验/YMODEM/会话/决策/入口）
├── bl_port.h            移植契约：要实现的 15 个函数
├── bl_port_stm32f4.cpp  STM32F4 现成实现（换芯片照它再写一份）
└── bl_config.h          编译期配置：Flash 分区 + 通信参数
```

没有子目录。**要用库 = 拷这 5 个文件**（`bl_port.h` 只是契约，可以不拷进工程）。

---

## 怎么用（三步）

**1. 改 `bl_config.h`** —— 只有 4 个数要按你的芯片填：

```c
BL_FLASH_SIZE    512UL * 1024UL     /* Flash 容量 */
BL_META_SIZE     128UL * 1024UL     /* 最后一个扇区的大小 */
BL_SRAM_BASE     0x20000000UL
BL_SRAM_END      0x20018000UL       /* 96KB，不含 */
```

**2. 让工程提供 `SystemClock_Config()`** —— 库不配时钟。
CubeMX 生成的工程天然有这个函数；纯手写工程就自己写一个（参考示例工程的
`Core/Src/bl_clock.c`，用的是不依赖晶振的 HSI+PLL）。

**3. 编进去** —— 库自带 `main()`，直接调 `blEntry()`，编好烧写即可。
接进已有工程时，把工程自带的 `main.c` 移出编译。

APP 工程要改两处：IROM1 起始 = `BL_APP_BASE`（默认 `0x08004000`），
`main()` 开头设 `SCB->VTOR = BL_APP_BASE`。

---

## 执行流程（图解 + 核心代码）

### 全景：谁调用谁

```
复位 → startup_stm32f401xe.s → SystemInit → main()
                                                └─ bl::blEntry()
                                                     ├─ ① chipInit()      芯片底座
                                                     ├─ ② uartInit()      串口
                                                     ├─ ③ flashInit()     Flash
                                                     ├─ ④ crcSelftest()   校验器自检
                                                     ├─ ⑤ meta().init()   读配置区
                                                     ├─ ⑥ layoutCheck()   分区对齐
                                                     ├─ ⑦ banner()        打印
                                                     ├─ ⑧ Boot::decide()  ★ 核心决策
                                                     └─ ⑨ IAP 循环        Session::run()
```

读代码从 `bl.cpp` **最末尾**开始 —— `main()` 在文件最后，只有一句 `bl::blEntry()`。

**全库唯一能把控制权交给 APP 的地方是 `jumpToApp()`**。其余路径要么停在
`fatal()`，要么回到 IAP 循环。这是"跑不飞"的结构性保证，不是靠小心。

### 决策：`Boot::decide()` —— 五级阶梯

`bl.cpp` ⑧ 段，完整代码就这些：

```cpp
Meta& m = meta();

// 复位原因必须每次启动都读（read-and-clear），否则旧标志会累积到下次启动
const ResetCause cause = resetCause();

// 1. 固件状态不可跳转 → 进 IAP
if (!m.shouldBoot()) {
    return { Action::EnterIap, "no bootable firmware" };
}

// 2. 软件复位唤回：APP 运行中软复位，进限时升级窗口
if (cause == ResetCause::Software) {
    return { Action::EnterIapTimed, "soft reset -> upgrade window" };
}

// 3. 向量表校验
if (!ok(verifyVectorTable(cfg.appBase, nullptr))) {
    return { Action::EnterIap, "invalid vector table" };
}

// 4. 整镜像 CRC32 校验（可选）
if (cfg.verifyCrcOnBoot) {
    const Meta::Slot& s = m.current();
    if (!ok(verifyImage(cfg.appBase, s.fwSize, s.fwCrc32, nullptr))) {
        return { Action::EnterIap, "image crc mismatch" };
    }
}

// 5. 全部通过 → 跳 APP
return { Action::JumpToApp, "ok" };
```

对应到现场：

| 现场情况 | 命中第几条 | 串口打印 | 结果 |
|---|---|---|---|
| 从没刷过固件 | 1 | `decision: IAP (no bootable firmware)` | 停在 IAP |
| 刷到一半掉电 | 1 | 同上（状态是 DOWNLOAD） | 停在 IAP，可重刷 |
| 正常运行按复位键 | 3、4 | `reset cause = 2 (pin)` + `decision: JUMP` | **零等待**跳 APP |
| APP 里软复位唤回 | 2 | `reset cause = 3 (sft)` + `decision: IAP_TIMED` | 进 15s 窗口等上位机 |
| 固件被擦掉半截 | 4 | `decision: IAP (image crc mismatch)` | 停在 IAP |

三个刻意的设计点：

- **顺序就是设计**。第 1 条在最前：状态不可信就绝不放行，哪怕向量表看起来没问题。
- **第 2 条限定 `Software`**。升级完成后的复位也是软件复位 —— 但那时状态是
  `Download`（还没 commit 完），命中的是第 1 条，不会误进窗口。
- **`resetCause()` 每次启动必读**。它是 read-and-clear 语义，漏读会让旧标志
  累积到下次启动，导致误判。

### 跳转：`Boot::jump()` → `jumpToApp()`

```cpp
void Boot::jump(uint32_t appBase) noexcept
{
    BL_LOG("[boot] jumping to app @ 0x%08lX\r\n", ...);
    jumpToApp(appBase);
    BL_LOG("[boot] jump failed!\r\n");   // 走到这里说明跳转没成功
}
```

`jumpToApp()` 在 port 层，八步顺序不能乱：

| 步 | 动作 | 漏掉的症状 |
|---|---|---|
| 1 | `__disable_irq()` 关总中断 | APP 里残留中断打进来，跑飞 |
| 2 | 停 SysTick（CTRL/LOAD/VAL 清零） | 跳过去立刻被旧 SysTick 中断打断 |
| 3 | 清 PendSV / PendST 挂起位 | 同上 —— SysTick 与 PendSV 不在 NVIC 里，得单独清 |
| 4 | `HAL_RCC_DeInit()` 复位 RCC | —— |
| 4b | **再关一次 SysTick** | `HAL_RCC_DeInit()` 会把 SysTick 重新打开，这个坑很隐蔽 |
| 5 | 清 NVIC 使能与挂起（8 组） | 残留外设中断在 APP 里触发 |
| 6 | `SCB->VTOR = appBase; __DSB();` | APP 一进中断就跳回 Bootloader 的向量表 |
| 7 | `__set_MSP(向量表[0]); __ISB();` | 用着 Bootloader 的栈跑 APP，栈越界毁 SRAM |
| 8 | `__set_CONTROL(0)` + 跳入口 | APP 跑在非特权模式，写寄存器全被拒 |

### 留在 IAP：一次升级的完整链路

```
blEntry()
└─ Session::run()
   ├─ Ymodem::receive()              阻塞收，握手超时 = BL_YMODEM_HANDSHAKE_MS
   │  ├─ onFileStart()               → meta().markDownload()  ← 先标"不可信"
   │  │                                 （擦配置扇区，约 1 秒）
   │  ├─ 擦除 APP 区所需扇区
   │  ├─ onFileData()                → flashWrite() + 边收边算 CRC32
   │  └─ onFileEnd()                 → meta().commit(size, crc32, ver)
   └─ 成功 → Boot::jump(BL_APP_BASE)  直接跳新固件
```

顺序不可颠倒：**① 置 Download 必须在 ② 擦 APP 区之前**。反过来的话，擦除中途
掉电会留下「状态 Valid 但 APP 已是空片」的组合，上电直接跳进空片。

### 校验一共几处

| 位置 | 函数 | 判据 | 挡住什么 |
|---|---|---|---|
| 启动自检 | `crcSelftest()` | 已知向量算出的 CRC 与预期相符 | 校验器本身写错（"固件没问题却总判 CRC 错"） |
| 启动自检 | `layoutCheck()` | 三个分区基址分别落在扇区边界 | 分区配错 → 擦除连坐相邻区域 |
| 决策第 3 条 | `verifyVectorTable()` | 栈顶在 SRAM 内、入口在 APP 区内、最低位为 1 | APP 没编 / 编到错地址 / 空片 |
| 决策第 4 条 | `verifyImage()` | 整镜像 CRC32 与配置区记录一致 | Flash 位翻转、擦写不完整 |
| 每帧 | YMODEM CRC16 | 帧内 CRC16 + ACK/NAK | 传输误码 |

CRC32 的口径：**只覆盖固件原始长度 `size`**，不含末包填充字节（不同工具填
`0x00` / `0x1A` / `0xFF` 不一样）。

---

## 掉电安全：为什么这个顺序不会变砖

| 掉电时刻 | 配置区状态 | 上电行为 |
|---|---|---|
| 置 Download 之前 | `INVALID` / `VALID` | 正常（原固件未动） |
| 置 Download 之后 | `DOWNLOAD` | 停在 IAP，可重刷 |
| 擦除 APP 中途 | `DOWNLOAD` | 停在 IAP，可重刷 |
| 传输数据中途 | `DOWNLOAD` | 停在 IAP，可重刷 |
| CRC 校验失败 | `DOWNLOAD` | 停在 IAP，可重刷 |
| 配置区擦除中途 | 全 `0xFF` = `ERASED` | 停在 IAP，可重刷 |

配置区是**单槽原地擦写**：状态变更就「擦整片 → 写一条」。擦除途中掉电只会让
记录变成全 FF（等于 Invalid），不会留下半个状态。

---

## APP 侧要做什么

**只有一件可选的事**：想支持"运行中被刷"，就在串口收齐关键字后软复位。

```c
#include "bl.h"          /* 只用到 blRequestUpdate，零依赖 */

/* 在你的串口接收处理里逐字节匹配，匹配完整才调用 */
if (匹配到 BL_BOOT_MAGIC_STRING) {
    blRequestUpdate();       /* 写 SCB->AIRCR 触发软件复位 */
}
```

关键字是 `#Bootloader-Everywhere`（`BL_BOOT_MAGIC_STRING`）。
样板见 `TestApp/app_main.c`（test-app 分支，纯寄存器实现，连 CMSIS 都不用）。

**不需要**"确认固件"、"喂狗"之类的动作。

---

## 移植到别的芯片

1. 复制 `bl_port_stm32f4.cpp` 为 `bl_port_<你的平台>.cpp`
2. 改顶部「板级配置」区（串口用哪一路、哪几个脚）
3. 照 `bl_port.h` 的注释实现那 15 个函数
4. `bl.cpp` 一个字都不用改

**清单**（15 个）：`chipInit` `flashInit` `flashSectorSize` `flashBytesToSectorEnd`
`flashErase` `flashWrite` `flashRead` `uartInit` `uartRead` `uartWrite` `uartFlushRx`
`jumpToApp` `tickMs` `delayMs` `resetCause`

**铁律**：

- `flashErase()` 必须拒绝擦除 Bootloader 自身区域（最后一道保护）
- `resetCause()` 必须「读后即清」，且软件复位优先于引脚复位
  （在线探针连着时两者会同时置位）
- 所有「等标志位」的循环都要带超时

不需要提供时钟配置 —— 那是宿主工程的事（见「怎么用」第 2 步）。

---

## 常见坑（都真踩过）

| 现象 | 原因 / 对策 |
|---|---|
| 串口全是乱码 | 时钟配错。HAL 按 `SystemClock_Config` 写下的分频值反算波特率，配错则偏差成倍 |
| 软复位唤回进不去窗口 | `resetCause()` 里**软件复位必须优先于引脚复位**（探针会连带拉 NRST） |
| APP 完全不输出 | APP 少做了事：重定位 `SCB->VTOR`、使能 FPU、清残留 SysTick |
| ROM 超 16KB | 别链接标准 `printf`：`vsnprintf` 连带浮点格式化吃掉约 6.5KB。用库里的 `BL_LOG` |
| 改了代码但行为没变 | **Keil 的 `-f` 不保证重编改动过的文件**，验证前必须 `-r` 全量重建 |
| 上位机读串口错位 | 日志与协议字节（`C`/ACK）共用串口；上位机要容忍夹在中间的文本，别盲等单字节 |
| 时钟一直是 16MHz | 宿主没提供 `SystemClock_Config()`。串口照样通（HAL 按实际时钟算），但慢；开机日志里 `sysclk=` 能看出来 |

---

## 占用与编译要求

- **C++11**（Keil：Options → C/C++ → Language 选 C++11）
- **库本体 ROM ≈ 12.3 KB**（`BL_DEBUG_LOG=1`）；`BL_DEBUG_LOG=0` 可再省约 2KB
  - 不含宿主的 `SystemClock_Config()`（那是 HAL 的事，实测约 1.3KB）
- 栈建议 ≥ 2KB（`Session` 带 1KB YMODEM 缓冲）
- 不依赖动态内存、异常、RTTI
