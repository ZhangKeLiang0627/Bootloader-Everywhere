# Bootloader-Everywhere

串口 IAP Bootloader，可移植库。给一个 bin 就能刷进 APP 区，**任何时刻断电都不会变砖**。

- 协议：自定义 0xA5 帧（见 `docs/PROTOCOL_DESIGN.md`）；命令行上位机 `TestApp/tools/proto.py`
- ⚠️ 网页版上位机（`web` 分支）仍是旧的 YMODEM 实现，**与本固件不兼容**；适配版在 `web-v2` 分支
- 库不初始化芯片，也不带 `main()` —— 芯片由宿主工程带起来，库只被 `blRun()` 调一次
- 不依赖 stdio，不用动态内存，C++11 无异常无 RTTI

## 库就是这几个文件

```
Bootloader/
├── bl.h                 对外头文件：blRun() + blUartRx() + Status（C 工程也能 include）
├── bl.cpp               主体：CRC32 / 向量表校验 / IAP 命令 / 决策 / 入口
├── bl_log.h             日志：开关 + BL_LOG 宏 + 接口声明
├── bl_log.cpp           日志实现：轻量格式化，不依赖 stdio
├── protocol.h           载体层：Frame / Parser / encode / crc8（与业务无关）
├── protocol.cpp         载体层实现：0xA5 帧编解码 + CRC8
├── bl_port.h            移植契约：12 个函数 + 一张扇区表
├── bl_port_stm32f4.cpp  STM32F4 现成实现（换芯片照它再写一份）
└── bl_config.h          分区参数 + 协议参数
```

没有子目录。**要用库 = 拷这 9 个源文件**（头文件是声明，按需要拷）。

`protocol.*` 是**通用载体层** —— 只依赖 `stdint.h`，不认识芯片也不认识业务，
可以整对拷到 APP、上位机、别的工程里复用。改帧格式看 `protocol.*`，改升级流程看 `bl.cpp`，
改日志看 `bl_log.*`。

帧长必须落在 `[proto::kFrameMin, proto::kFrameMax]`（**7 - 1031 字节**，空载荷帧到满载荷帧）。
`Parser` 与 `Frame::lenOk()` 都会校验：长度字段被噪声改坏时直接丢帧，让主机重传。

## 怎么用（三步）

**1. 改 `bl_config.h`** —— 只有几个数要按你的芯片填：

```c
BL_FLASH_SIZE    512UL * 1024UL     /* Flash 容量 */
BL_BOOT_SIZE     16UL * 1024UL      /* Bootloader 区，正好一个扇区 */
```

SRAM 不用配 —— 上电判据只看「SP 是不是 0x20000000 起的 8 字节对齐地址」，
这是 Cortex-M 的架构约定，与芯片容量无关。

APP 区地址与大小都是派生值：`BL_APP_BASE = BL_FLASH_BASE + BL_BOOT_SIZE`。

**2. 宿主工程把芯片带起来** —— 时钟、串口、Flash 接口时钟都由宿主初始化，
库一行初始化都不做。CubeMX 生成的工程天然满足（`main.c` / `usart.c` / `gpio.c` /
`stm32f4xx_it.c` / `stm32f4xx_hal_msp.c` 全部保持原样）。

**3. 在宿主 main 里调库**：

```c
#include "bl.h"

int main(void)
{
    HAL_Init();
    SystemClock_Config();       /* 时钟：宿主自己的 */
    MX_GPIO_Init();
    MX_USART1_UART_Init();      /* 串口：8N1，波特率与上位机一致（默认 115200） */

    blRun();                    /* 永不返回：要么在 IAP 里等，要么跳 APP */
    for (;;) { }
}
```

`blRun()` 要放在 `while` **之前** —— 它不会返回。

## 提交机制：靠「写入顺序」，不靠 Flash 存状态

APP 区最前面两个字是向量表的 **SP**（初始栈顶）和 **PC**（复位入口），
上电的判据就是「这两个字合法吗」。关键在于**让这两个字最后才写**：

```
擦除 APP 区
  → 收第 1 包：把最前面 8 字节（SP、PC）扣在 RAM 里，其余照写
  → 收第 2..N 包：照写
  → 回读 [8, size) 重算 CRC32，与接收时算的比对   ← 确认 Flash 真写对了
  → 写 SP（第 1 个字）
  → 写 PC（第 2 个字）                            ← 提交点，过了这里才可启动
```

于是掉电在每个时刻都自证清白：

| 断在哪 | 上电读到 | 结果 |
|---|---|---|
| 擦除前 | 旧固件向量表 | 跳旧固件 |
| 擦除中 / 擦完没写 | SP=0xFFFFFFFF | 停 IAP，可重刷 |
| 写第一包的其余 1016 字节时 | SP/PC 仍是 0xFF | 停 IAP，可重刷 |
| 收后续包时 | 同上 | 停 IAP，可重刷 |
| 回读校验时 | 同上 | 停 IAP，可重刷 |
| 写完 SP、还没写 PC | PC=0xFFFFFFFF | 停 IAP，可重刷 |
| 写完 PC | 完整向量表 | 跳新固件 |

**为什么不能把整包 1KB 都扣到最后写**：那样"写到一半"时 SP/PC 已经落盘，
上电会误判为完整固件而跳进半截程序。只扣这 2 个字就没这个洞。

**代价**：APP 区不需要保留任何空间，但不再做「启动时整镜像 CRC」——
完整性由提交前的回读校验负责（比"下次上电再校验"更强：写入失败当场暴露）。

**收益**：不需要配置区 / 状态机 / 魔术数，不占 Flash，不额外擦扇区，启动零等待。

## 怎么进入 IAP（三种场合）

| 场合 | 做法 |
|---|---|
| 板子上有按钮 | **按住按钮上电** → 直接停 IAP。上电读一次电平，零等待，不需要 APP 配合 |
| APP 正在正常跑 | 上位机发关键字 → APP 软复位 → 15s 限时窗口 |
| APP 区是空的 / 传输没提交 | 上电自动停 IAP |

按钮那条：库调 `bl_port.h` 的 `bootPinHeld()` 读电平，引脚与极性在
`bl_port_stm32f4.cpp` 顶部四行宏里配；引脚本身由宿主配成「输入 + 上拉」
（本仓库见 `Core/Src/main.c` 的 `bootPinInit()`）。松开按钮重新上电即正常启动。
板子上没有按钮：把那四行宏注释掉，`bootPinHeld()` 自动返回 false。

## 升级流程（正常一次）

```
上位机                                    Bootloader
  │ 发 "#Bootloader-Everywhere"（APP 收到后软复位）
  │                                        上电 → 复位原因 = 软件复位
  │                                        → 向量表合法 → 进 15s 唤回窗口
  │ START: size / crc32 / sp / pc ────────► 校验向量表 → 擦除 APP 区（数秒）
  │ ◄───────── OK + blockSize ────────────
  │ DATA: addr / total / index / vlen /
  │       cumCrc32 / 数据（512 B 一帧）───► 交叉校验头部 → 比累积 CRC32
  │                                        → 写 Flash → 读回重算 → 应答
  │ ◄──── OK + cumCrc32 + 期望地址 ───────   （错帧回 NAK，主机从期望地址续传）
  │ END ──────────────────────────────────► 总 CRC32 比对 → 整片回读校验
  │                                        → 写 SP/PC（提交）→ 应答 → 跳 APP
```

窗口内没有上位机来（15s）→ 自动跳回 APP，不用重新上电。

## 移植（换芯片）

写 `bl_port.h` 的 12 个函数 + 一张扇区表，`bl.cpp` 一个字都不用改：

```
flashErase  flashWrite  flashRead
uartRead  uartWrite  uartFlushRx  uartRxIrqHandler
tickMs  delayMs
resetCause  jumpToApp
bootPinHeld         （没按钮的直接 return false）

kFlashSectors[]    扇区表（bl_port.h 的 struct FlashSector）
```

扇区表就是「S0 在 0x08000000 占 16KB、S4 是 64KB、S5 起每个 128KB」这样的直白列表
（`bl_port_stm32f4.cpp` 里现成一份，地址一眼可查）。**怎么按扇区走由库负责，
port 只提供事实** —— 所以表在 `bl_port.h` 里声明、port 里定义。

三条约定：

- 失败一律 `return Status::...`，不用异常、不用动态内存
- `flashErase` 必须拒绝擦除 Bootloader 自身区域（最后一道保护）
- 所有「等标志位」的循环都要带超时

宿主必须先初始化好：时钟、串口（8N1，波特率与上位机一致）、Flash 接口时钟。
port 用 HAL 收发串口，所以要能拿到宿主的句柄名 —— 板级配置区一行
`extern "C" UART_HandleTypeDef huart1;`（换了句柄名只改这一行）。
要用「按住按钮上电」还要把按钮引脚配成「输入 + 上拉」——**这步漏了按键就没反应**，
但不会误判（见下面「常见坑」）。
若宿主用中断收串口，IAP 期间要关掉收类中断 —— 库是轮询收的，
`uartFlushRx()` 里顺带关掉了 `RXNEIE / PEIE / EIE`。

## 常见坑

| 现象 | 原因 |
|---|---|
| 跳 APP 后串口零输出 | 跳转前没关 SysTick / 没复位 RCC。顺序见 `jumpToApp()` 注释（`HAL_RCC_DeInit()` 会重新打开 SysTick，所以关它必须放在后面） |
| 串口满屏乱码 | 宿主时钟配错 → HAL 算错波特率分频。串口是异步的，双方只能各自算准 |
| APP 一跑就 HardFault | APP 自己没使能 FPU（库不再代劳；CubeMX 的 `SystemInit()` 里有这段） |
| 上电永远停在 IAP | APP 区向量表非法（空片 / 上次传输没提交）。重新刷一次即可 |
| 按住按钮没反应 | 该 GPIO 端口时钟没开，或引脚没配成输入。库读到时钟未开就按「没按」处理（刻意如此，反向误判会让 APP 永远起不来） |
| 编译报重复定义 `SystemClock_Config` | 宿主里有两份，删掉一份 |
| ROM 超出 Bootloader 区 | 别链接标准 `printf`（连带浮点吃约 6.5KB），用 `BL_LOG` |

## 实测数据（STM32F401RET6）

| 项 | 值 |
|---|---|
| 整份固件（库 + 宿主 + HAL + CMSIS） | 13008 B，16KB 区余量 3376 B |
| 库本体（`tools/build.py` 量库 + port，不含宿主） | 8876 B |
| 关日志（`-DBL_DEBUG_LOG=0`） | 再省约 2.7KB |
| 真板回归 | `TestApp/tools/test_proto.py` T1-T5 全通过（5/5） |
