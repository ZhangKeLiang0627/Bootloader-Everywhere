# LUMOS-bootloader

一个**可搬走的串口 IAP Bootloader**。板子先跑它，它再决定是跳进你的 APP、
还是留在原地等新固件。

- 用 YMODEM-1K 协议经串口刷固件：接收 → 校验 → 跳转
- 运行中的 APP 可被网页/上位机一键唤回（发关键字 `#Bootloader-Everywhere`）
- 十几 KB 大小，零依赖、跨芯片：**同一份目录拷进任何 Cortex-M 工程都能用**

> 定位是 **IAP**（人站在板子前刷），不是 OTA。所以不做自动回滚、不用看门狗 ——
> 刷进坏固件就再刷一次，不为此增加运行期副作用。这是刻意的取舍。

---

## 库就是这几个文件

```
Bootloader/
├── README.md            ← 你正在看的这一份，唯一的文档
├── bl.h                 ← 唯一入口头：blRun() / blRequestUpdate()
├── bl.cpp               ← 全部实现（不自带任何芯片头文件）
├── bl_port.h            ← 移植契约：要实现的那些函数 + 填空说明
├── bl_port_stm32f4.cpp  ← STM32F4 现成实现（换芯片照它写一份）
└── bl_config.h          ← 芯片 / 分区 / 波特率等编译期配置
```

没有子目录，没有隐藏分层。**要用库 = 拷这 6 个文件。**

---

## 怎么用（三步）

**1. 拷目录 + 加 include 路径**

```
-I <你的工程>/Bootloader
```

**2. 加进编译**

| 加入编译 | 说明 |
|---|---|
| `bl.cpp` | 库本体，不用改 |
| `bl_port_stm32f4.cpp` | 移植实现，换芯片就换这一份 |
| 你的启动文件 + 芯片 HAL/CMSIS | 库依赖宿主的 HAL |

**3. 接入口**——二选一：

```c
/* 方式 A：让库自带 main()（默认，BL_PROVIDE_MAIN=1）
 *   什么都不用写，但要把宿主工程自带的 main.c 移出编译 */

/* 方式 B：接进你自己的 main() */
#include "bl.h"
int main(void) {
    my_stuff_init();
    blRun();              /* 不返回 */
}
```

宿主工程里需要**移出编译**的：自带 `main.c`、`stm32f4xx_it.c`、
`stm32f4xx_hal_msp.c`（库自带这些功能，否则重复符号）。

---

## 执行流程（图解 + 核心代码）

> 想读代码就从 **`bl.cpp` 的最末尾**开始 —— `main()` 在文件最后，它只有一句
> `bl::blEntry()`。下面按"复位之后实际发生的顺序"讲。

### 全景：谁调用谁

```
上电/复位
 └─ 启动文件 startup_stm32f401xe.s
     └─ SystemInit()                     HAL 的：时钟、FPU
         └─ __main → main()              ★ bl.cpp 文件末尾
             └─ blRun()                 extern "C"，C / C++ 工程都能调
                 └─ bl::blEntry()       ★ 真正的入口（不返回）
                     │
                     ├─ ① 五项自检 ──────────── 任一失败 → fatal() 停住
                     ├─ ② banner + dump ────── 打印芯片与分区（调试用）
                     ├─ ③ layoutCheck() ───── 分区是否落在扇区边界
                     ├─ ④ Boot::decide()      ★ 核心决策
                     │    ├─ 不可跳转 ──────────────────► 进 ⑤ IAP 循环
                     │    ├─ 软件复位 + Valid ──────────► 进 ⑤ IAP 循环（限时窗口）
                     │    └─ 校验全过 ──► Boot::jump() ─► jumpToApp() 八步
                     └─ ⑤ IAP 循环 Session::run()
                            └─ 升级成功 ──► Boot::jump()（和上面同一条路）
```

只有**一个地方**能让控制权交给 APP：`jumpToApp()`。其余所有路径要么停在
`fatal()`，要么回到 IAP 循环 —— 这就是"不会跑飞"的结构性保证。

---

### ① 五项自检（`blEntry()` 开头）

```cpp
[[noreturn]] void blEntry() noexcept
{
    if (!ok(platformInit()))             fatal("platform init failed");
    if (!ok(uartInit(BL_UART_BAUDRATE))) fatal("uart init failed");
    if (!ok(flashInit()))                fatal("flash init failed");
    if (!ok(crcSelftest()))              fatal("crc selftest failed (check poly/init)");
    if (!ok(meta().init()))               fatal("meta init failed");

    banner();
    meta().dump();
```

| 自检 | 干什么 | 失败为什么直接停 |
|---|---|---|
| `platformInit()` | 时钟树、1ms 时基、串口引脚 | 时钟都不对，后面所有事都没意义 |
| `uartInit()` | 8N1 串口 | 不能刷机、也打不出日志 |
| `flashInit()` | 解锁 Flash、开接口时钟 | 擦写会静默失败 |
| `crcSelftest()` | **自己验自己**：用已知数据算一遍 CRC16/CRC32，和期望值比 | CRC 实现错了（多项式/初值/位序），会导致"好固件被判成坏"或反过来 |
| `meta().init()` | 扫描配置区，装载最新有效槽 | 读不出固件状态，无法决策 |

`crcSelftest()` 值得单独说：**校验逻辑本身也要被校验**。它挡的是"校验器写错"
这类最难发现的问题 —— 否则你会看到"固件明明没问题却一直被判 CRC 错"。

---

### ② 分区检查 `layoutCheck()`

```cpp
bool partitionAligned(const char* name, uint32_t base) noexcept
{
    const uint32_t unit = flashSectorSize(base);
    if (unit == 0U) { /* 地址根本不在这颗芯片的 Flash 里 */ return false; }
    if (flashBytesToSectorEnd(base) != unit) {
        // 到下一扇区边界的字节数 != 本扇区大小 ⇒ base 不在扇区起点上
        return false;
    }
    return true;
}

bool layoutCheck() noexcept
{
    bool all = true;
    if (!partitionAligned("BOOT", BL_BOOT_BASE)) all = false;
    if (!partitionAligned("APP",  BL_APP_BASE))  all = false;
    if (!partitionAligned("META", BL_META_BASE)) all = false;
    return all;
}
```

**为什么必须查**：Flash 只能整扇区擦除。分区基址若差一个字节，
"擦 APP 区"就会把相邻区一起擦掉 —— 包括 Bootloader 自己。

**特别之处**：检查不通过时，如果固件本身是可启动的，仍然放它跑：

```cpp
    if (!layoutCheck()) {
        BL_LOG("[main] FATAL: partition layout invalid\r\n");
        if (meta().shouldBoot()) {
            Boot::jump(BL_APP_BASE);        // 分区地址配错 ≠ 固件坏，先让 APP 活下去
        }
        fatal("partition layout invalid");  // 否则停在报错，绝不进 IAP 做破坏性擦除
    }
```

理由：读操作永远安全，写操作（擦除）才危险。能跑就先跑，不能跑就停着等人。

---

### ③ 决策 `Boot::decide()` —— 五级阶梯（核心）

```cpp
Boot::Decision Boot::decide(const Config& cfg) noexcept
{
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
}
```

`shouldBoot()` 的判据只有一条：

```cpp
bool shouldBoot() const noexcept { return bootable(meta_.state); }   // state == Valid
```

**五种现场情况分别走哪条路**（对着串口日志就能对号入座）：

| 上电时的情况 | 命中第几条 | 串口会打印 | 结果 |
|---|---|---|---|
| 从没刷过固件（`Invalid`） | 1 | `decision: IAP (no bootable firmware)` | 停在 IAP，无限等刷机 |
| 刷到一半掉电（`Download`） | 1 | 同上 | 停在 IAP，可重刷（**不变砖**） |
| 正常跑着，用户按复位键 | 3、4 通过 | `reset cause = 2 (pin)` + `decision: JUMP (ok)` | **零等待**直接跳 APP |
| APP 里收到唤回关键字后软复位 | 2 | `reset cause = 3 (sft)` + `decision: IAP_TIMED` | 进 15s 限时窗口 |
| 固件区被擦掉半截（CRC 不符） | 4 | `decision: IAP (image crc mismatch)` | 停在 IAP，可重刷 |

两个刻意的设计点：

1. **顺序本身就是设计**。先判"能不能跳"，再判"要不要等上位机"，最后才做
   昂贵的校验（CRC32 要读整片 APP 区）。能省的活不干。
2. **第 2 条排在校验之前**。因为"有人按了升级"这件事的优先级高于"固件是否完好"
   —— 反正马上就要覆盖它了，没必要先花时间校验旧固件。
3. **`resetCause()` 每次启动都要读**。`RCC_CSR` 里的复位标志是**累积**的，
   只有读才清。若只在某个分支里读，标志会一直粘着，导致下次启动误判成"软件复位"。

---

### ④ 跳转到 APP：`Boot::jump()` → `jumpToApp()` 八步

```cpp
void Boot::jump(uint32_t appBase) noexcept
{
    BL_LOG("[boot] jumping to app @ 0x%08lX\r\n", static_cast<unsigned long>(appBase));
    jumpToApp(appBase);              // 移植层的函数，正常不返回
    BL_LOG("[boot] jump failed!\r\n");  // 只有跳转失败才会走到这
}
```

真正干活的是移植层的 `jumpToApp()`。**八步的顺序不能乱，一步都不能少**：

```cpp
void jumpToApp(uint32_t appBase) noexcept
{
    stm32f4::consoleTxFlush(100U);   /* 等最后几行日志发完再跳 */

    /* 取向量表前两字：初始栈顶与复位入口 */
    const uint32_t initialSp = *reinterpret_cast<volatile uint32_t*>(appBase);
    const uint32_t resetVec  = *reinterpret_cast<volatile uint32_t*>(appBase + 4U);

    BL_LOG("[jump] sp=0x%08lX entry=0x%08lX\r\n", ...);

    /* 1. 关全局中断 */
    __disable_irq();

    /* 2. 复位 RCC 到默认态（HSI）。APP 的 SystemInit 会按自己的配置重建 PLL。
     *    ⚠️ HAL_RCC_DeInit() 内部末尾会调 HAL_InitTick()，
     *       也就是**重新把 SysTick 配成 1ms 并使能中断** ——
     *       所以「关 SysTick」必须放在它后面！ */
    HAL_RCC_DeInit();

    /* 3. 关 SysTick 并清挂起位 */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

    /* 4. 清所有 NVIC 中断使能与挂起标志 */
    for (uint32_t i = 0; i < 8U; ++i) {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    /* 5. 重定位向量表到 APP，并保证对后续取指立即生效 */
    SCB->VTOR = appBase;
    __DSB();

    /* 6. 设置主堆栈指针 */
    __set_MSP(initialSp);

    /* 7. 回到特权级 + 使用 MSP（若此前用过 PSP） */
    __set_CONTROL(0U);
    __ISB();

    /* 8. 开中断并跳转 */
    __enable_irq();
    using AppEntry = void (*)(void);
    auto entry = reinterpret_cast<AppEntry>(resetVec);
    entry();                            // ← 控制权在这一行交给 APP

    for (;;) { }                        // 正常不会到这
}
```

**每一步漏掉会怎样**（这些症状都不好猜，所以别删）：

| 步 | 动作 | 漏掉的症状 |
|---|---|---|
| 1 | `__disable_irq()` | 跳转途中被中断打断，跳到半路 |
| 2 | `HAL_RCC_DeInit()` | APP 以为时钟还是 Bootloader 配的 → 串口波特率全错（乱码） |
| 3 | 关 SysTick + 清挂起 | APP 一跑就被 SysTick 打断，而它的 `SysTick_Handler` 是空的（`B .`）→ **APP 完全不输出** |
| 4 | 清 NVIC | APP 一开中断就冲进一个已挂起的中断服务函数 |
| 5 | `SCB->VTOR` | APP 的中断打进 Bootloader 的向量表 |
| 6 | `__set_MSP()` | 栈指针还指着 Bootloader 的栈，一压栈就踩坏数据 |
| 7 | `__set_CONTROL(0)` | 若之前用过 PSP，APP 会在错误的栈上跑 |
| 8 | 取 `resetVec` 并调用 | —— 这一步才是真的跳过去 |

三个容易踩的顺序/细节陷阱：

- **第 2 步必须在第 3 步之前**（`HAL_RCC_DeInit()` 会把 SysTick 重新打开）。
- **`SysTick` / `PendSV` 不在 NVIC 里**（它们是系统异常），
  所以第 4 步那个"清 NVIC"的循环管不到它们，必须像第 3 步那样单独清挂起位。
- **`__set_MSP` 之后要 `__ISB()`**，保证后面的取指用新栈。

---

### ⑤ 留在 IAP：一次升级的完整调用链

```
blEntry()
 └─ for (;;) {
      BL_LOG("[main] waiting for YMODEM transfer...");
      Session session;
      session.run()                          ← ⑥ 会话（bl.cpp）
        └─ ymodem_.receive()                 ← ⑤ 协议（bl.cpp）
             ├─ 发 'C' 请求 CRC 模式
             ├─ 收到首包 → sink_.onFileStart(name, size)
             │     ├─ 校验 size ≥ 1 且 ≤ BL_APP_SIZE
             │     ├─ meta().markDownload()          ① 先置 Download（不可信）
             │     ├─ eraseRegion(writeLen)          ② 按扇区擦除
             │     └─ 回 ACK + 'C'（此刻擦除已做完，PC 还在等 ACK，不会超时）
             ├─ 收到数据包 → sink_.onFileData(offset, data, len)
             │     ├─ flashWrite(BL_APP_BASE + offset, data, len)
             │     └─ crc_.update(data, crcLen)        ④ 边收边算 CRC32
             ├─ 收到 EOT → sink_.onFileEnd(total)    记录本端算出的 CRC32
             └─ 收结束帧（全零首包）
        │
        ├─ 校验：收到的字节数 ≥ 声明大小        挡"半截文件被当成完整固件"
        ├─ verifyVectorTable()              ④ 确认刷进去的确实能启动
        └─ meta().commit(size, crc32, version) ⑤ 置 Valid + 记录指纹
      │
      └─ outcome == Done → delayMs(300) → Boot::jump(BL_APP_BASE)
                                             ↑ 和 ④ 同一条路径，只有一条跳转路
    }
```

对应代码（`Session::run()` 的后半段）：

```cpp
    /* 确认固件大小与声明一致，防止半截文件被当成完整固件 */
    if (out.stats.received < declared_size_) {
        result_.outcome = IapResult::Failed;
        result_.error   = Status::Protocol;
        return result_;
    }

    /* 校验向量表，确保刷进去的东西确实能启动 */
    if (!ok(verifyVectorTable(cfg_.appBase, nullptr))) {
        result_.outcome = IapResult::Failed;
        result_.error   = Status::CrcFail;
        return result_;
    }

    /* 提交：置 Valid 并记录 size / crc32 / 版本 */
    if (!ok(meta().commit(result_.fwSize, result_.fwCrc32, result_.version))) {
        result_.outcome = IapResult::Failed;
        result_.error   = Status::FlashFail;
        return result_;
    }

    result_.outcome = IapResult::Done;
```

**注意顺序**：先 `verifyVectorTable()` 再 `commit()`。
校验不过就不 commit，状态留在 `Download` —— 下次上电会判为"不可跳转"，
停在 IAP 等重刷。这样坏固件永远不会被跳转。

---

### ⑥ 校验一共几处、各自挡什么

| 位置 | 函数 | 判据 | 挡住什么 |
|---|---|---|---|
| 启动决策第 3 条 | `verifyVectorTable()` | 栈顶落在 `[BL_SRAM_BASE, BL_SRAM_END]`；入口落在 APP 区内且最低位为 1（Thumb） | 刷进去的东西根本不是一个能启动的镜像（错地址、空片、误烧） |
| 启动决策第 4 条 | `verifyImage()` | 重算 Flash 的 CRC32 == 配置区记录值 | Flash 位翻转、擦写不完整 |
| 升级收完时 | `verifyVectorTable()` | 同上 | 传坏/填错的固件，**不 commit**，状态停在 Download |
| 每个数据帧 | YMODEM 帧 CRC16 | 帧 CRC 正确才回 ACK | 串口传输误码（错了就让上位机重传） |
| 上电自检 | `crcSelftest()` | 已知输入 → 期望输出 | **校验器自己写错**（最隐蔽的一类） |

向量表校验的实际判据（`verifyVectorTable()`）：

```cpp
    const uint32_t initialSp    = vec[0];   /* 向量表第 0 字：初始栈顶 */
    const uint32_t resetHandler = vec[1];   /* 向量表第 1 字：复位入口 */

    const bool spOk =
        (initialSp >= BL_SRAM_BASE) && (initialSp <= BL_SRAM_END);

    const bool entryOk =
        (resetHandler >= BL_APP_BASE) &&
        (resetHandler <  (BL_APP_BASE + BL_APP_SIZE)) &&
        ((resetHandler & 0x1U) != 0U);      /* Thumb 地址最低位必须是 1 */

    return (spOk && entryOk) ? Status::Ok : Status::CrcFail;
```

一个必须与上位机对齐的约定（`verifyImage()` 注释原文）：

> **CRC32 只覆盖固件原始长度 `size`，不做任何对齐取整。**
> YMODEM 按 1024 字节分包，最后一包不足时发送方会用填充字节补满
> （不同工具的填充值可能是 `0x00` / `0x1A` / `0xFF`），这些填充字节同样会被
> 写进 Flash。若上位机按"原始文件字节"算、而本端按"对齐后长度"算，
> 结果必然对不上。因此双方统一：**CRC 只覆盖前 `size` 字节**。

---

## 掉电安全：为什么这个顺序不会变砖

固件状态只有三个：

| 状态 | 什么时候 | 上电后的结果 |
|---|---|---|
| `Invalid` | 配置区空 / 没刷过 | 留在 IAP |
| `Download` | 正在写，或写到一半掉电 | 留在 IAP（可重刷） |
| `Valid` | 写完且校验通过 | 跳转 APP |

升级的原子顺序（**① 必须在 ② 之前**）：

```
① 状态置 Download（先让固件"不可信"）
② 擦除 APP 区
③ 接收并写入
④ 校验向量表 + 整镜像 CRC32
⑤ 状态置 Valid → 直接跳新固件
```

顺序写反（先擦再置标志）时，擦除中途掉电会留下"状态 Valid 但 APP 是空片"
的组合 —— 那才是真变砖。按上面的顺序，任意时刻掉电都安全：

| 掉电时刻 | 配置区状态 | 上电行为 |
|---|---|---|
| 置 Download 之前 | `Invalid` / `Valid` | 正常（原固件没动） |
| 置 Download 之后 | `Download` | 停在 IAP，可重刷 |
| 擦除 APP 中途 | `Download` | 停在 IAP，可重刷 |
| 传输数据中途 | `Download` | 停在 IAP，可重刷 |
| 校验失败 | `Download` | 停在 IAP，可重刷 |


---

## APP 侧要做什么

**只有一件可选的事**：想支持"运行中被刷"，就在串口收齐关键字后软复位。

```c
#include "bl.h"          /* 只用到 blRequestUpdate，零依赖 */

/* 在你的串口接收处理里逐字节匹配，匹配完整才调用 */
if (匹配到 BL_BOOT_MAGIC_STRING) {
    blRequestUpdate();     /* 写 SCB->AIRCR 触发软件复位 */
}
```

样板见 `TestApp/app_main.c`（test-app 分支，纯寄存器实现）。
**不需要**"确认固件"、"喂狗"之类的动作。

---

## 移植到别的芯片

### 先分清两种情况

**情况一：同族芯片（F401/F405/F407/F411/F415/F417）**
扇区规则与编程粒度相同 → **只改 `bl_config.h` 第一节**（已有预置），
`bl_port_stm32f4.cpp` 直接复用。

**情况二：异族芯片（GD32 / CH32 / 其他 Cortex-M）**
复制 `bl_port_stm32f4.cpp` 为 `bl_port_<平台>.cpp`，照 `bl_port.h` 的注释
逐项实现，并在 `bl_config.h` 的「手填区」填容量与时钟。差异通常在这三处：

| 项 | STM32F4 | GD32F303 |
|---|---|---|
| 擦除单位 | 扇区，**不等长**（16/64/128KB） | 页，**等长**（2KB） |
| 编程单位 | 32 位字 | 16 位半字 |
| 时钟树 | PLL 从 HSE 分频后倍频 | PLL 从 PREDV 输出倍频，切换前要降 AHB |

> **RISC-V 内核（如 CH32V307）注意**：向量表寄存器不是 `SCB->VTOR`，
> 跳转流程也不同，必须查该芯片手册，不能照搬 Cortex-M 的八步。

### 换芯片时的铁律

**`bl.cpp` 一个字都不改。** 它只按"字节地址 + 字节长度"提要求，
不知道擦除单位是扇区还是页。验证方法很简单：

```bash
grep -E '#include *[<"](stm32|gd32|ch32|hal)' Bootloader/bl.cpp    # 应该没有任何输出
```

### 上板前自查

- [ ] 编译期自检通过（分区不重叠 / 配置区基址落在扇区边界 / `HSE_VALUE` 一致）
- [ ] 链接后 ROM 装得进 `BL_BOOT_SIZE`（用 `tools/build.py` 量）
- [ ] 上电能看到 banner，boot/app/meta 三个地址符合预期
- [ ] 分区检查没报警（`not on a sector boundary`）
- [ ] 空片时停在 `waiting for YMODEM transfer...`，不是乱跳
- [ ] 能刷成功一次，复位后能跳进 APP
- [ ] 刷到一半拔电重来，仍能进 IAP

---

## 常见坑（都真踩过）

| 现象 | 原因 / 对策 |
|---|---|
| 串口全是乱码 | `HSE_VALUE` 与 `bl_config.h` 的 `BL_HSE_HZ` 不一致。HAL 按 HSE 反算波特率，差一点就全错。库有 `#error` 挡 |
| 升级永远失败、`markDownload failed` | 配置区里有**旧格式/损坏的残槽**，偏移 0 没被擦除，写不进去。库已在 `Meta::init()` 里处理（发现脏槽先擦整片重来）—— 换芯片改槽结构时别把这个分支改掉 |
| 软复位唤回进不去窗口 | 在线探针连着时，`SYSRESETREQ` 会连带置引脚复位标志，两者同时有效。`resetCause()` 里**软件复位必须优先于引脚复位**（库里已如此） |
| APP 完全不输出 | 多半是 APP 自己少做了事：重定位 `SCB->VTOR`、使能 FPU、清残留 SysTick。见 `TestApp/README.md` |
| ROM 直接超 16KB | 别链接标准 `printf`：`vsnprintf` 会连带浮点格式化吃掉约 6.5KB。用库里的 `BL_LOG` |
| 等待循环卡死 | 所有"等标志位"的循环都要带超时，硬件异常时也不能死等 |
| 上位机读串口错位 | 调试日志与协议字节（`C`/ACK）共用同一串口；上位机必须容忍夹在中间的文本，别盲等单字节 |

---

## 占用与编译要求

| 项 | 值 |
|---|---|
| 编译器 | ARM Compiler 6（AC6）/ Keil MDK |
| C++ 标准 | **C++11**（Keil：Options → C/C++ → Language C++ 选 c++11） |
| 异常 / RTTI / 动态内存 | 全部关闭（`-fno-exceptions`，无 RTTI，无 new/malloc） |
| 优化 | `-Oz` |
| ROM | **13.85 KB**（F401 实测，16 KB 余量约 2.2 KB） |
| RAM | 静态约 430 B（`Session` 带 1 KB YMODEM 缓冲在栈上，栈别小于 2 KB） |
