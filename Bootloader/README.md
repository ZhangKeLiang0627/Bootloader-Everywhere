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
├── bl.h                 ← 唯一入口头：bl_run() / bl_request_update()
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
    bl_run();              /* 不返回 */
}
```

宿主工程里需要**移出编译**的：自带 `main.c`、`stm32f4xx_it.c`、
`stm32f4xx_hal_msp.c`（库自带这些功能，否则重复符号）。

---

## 上电后会发生什么

```
上电
 └─ 平台自检（时钟/串口/Flash）
     └─ 分区布局检查  ── 不通过 ──► 停在报错（绝不做破坏性擦除）
         └─ 读配置区，判断固件状态
             ├─ Valid ──┐
             │          ├─ 复位原因是「软件复位」？ ── 是 ──► 进限时窗口（15s）
             │          │                                      等 YMODEM；超时跳回 APP
             │          └─ 否 ──► 零等待，直接跳 APP
             └─ Invalid / Download ──► 留在 IAP，无限等 YMODEM
```

固件状态只有三个：

| 状态 | 什么时候 | 结果 |
|---|---|---|
| `Invalid` | 配置区空 / 没刷过 | 留在 IAP |
| `Download` | 正在写，或写到一半掉电 | 留在 IAP（可重刷，**不会变砖**） |
| `Valid` | 写完且校验通过 | 跳转 APP |

**升级的原子顺序**（掉电安全就靠这个顺序）：

```
① 状态置 Download（先让固件"不可信"）
② 擦除 APP 区
③ 接收并写入
④ 校验向量表 + 整镜像 CRC32
⑤ 状态置 Valid → 直接跳新固件
```

第 ① 步必须在 ② 之前。反过来的话，擦除中途掉电会留下"状态可跳转但 APP 是空片"
的组合 —— 那才是真变砖。

---

## APP 侧要做什么

**只有一件可选的事**：想支持"运行中被刷"，就在串口收齐关键字后软复位。

```c
#include "bl.h"          /* 只用到 bl_request_update，零依赖 */

/* 在你的串口接收处理里逐字节匹配，匹配完整才调用 */
if (匹配到 BL_BOOT_MAGIC_STRING) {
    bl_request_update();     /* 写 SCB->AIRCR 触发软件复位 */
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
| 升级永远失败、`mark_download failed` | 配置区里有**旧格式/损坏的残槽**，偏移 0 没被擦除，写不进去。库已在 `Meta::init()` 里处理（发现脏槽先擦整片重来）—— 换芯片改槽结构时别把这个分支改掉 |
| 软复位唤回进不去窗口 | 在线探针连着时，`SYSRESETREQ` 会连带置引脚复位标志，两者同时有效。`reset_cause()` 里**软件复位必须优先于引脚复位**（库里已如此） |
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
