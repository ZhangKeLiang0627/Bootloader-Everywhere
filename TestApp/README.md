# TestApp —— LUMOS-bootloader 的测试 APP

一个**最小但真实**的 APP，用来验证 Bootloader 的完整升级链路：

```
Bootloader 启动 → 等 YMODEM → 接收固件 → 擦除/写入 APP 区 → CRC 校验
    → 状态置 TESTING → 复位 → 重新决策 → 跳转到 APP → APP 正常运行
```

它不是"为了测试而拼凑的假件"，而是一个**能独立运行的真实 APP**：
即使不经过 Bootloader、直接烧到 `0x08004000`，它也能跑起来。
能跑通才说明跳转是真的成功。

## 目录

```
TestApp/
├── README.md            本文档
├── app_main.c           APP 主体（寄存器直接驱动，不依赖 HAL/CubeMX）
├── startup_app.s        启动文件（取自 ST 的标准 startup，栈 4KB / 堆 0）
├── link_app.sct         链接脚本：整个镜像从 0x08004000 开始，上限 368KB
├── build_app.py         构建脚本（armclang + armasm + armlink）
└── tools/
    └── ymodem_send.py   YMODEM-1K 上位机（也是将来网页版的协议参照）
```

## 构建

```bash
python TestApp/build_app.py
# 产物：build/app_test.bin / app_test.hex
```

依赖 Keil MDK 的 AC6 工具链（`armclang` / `armasm` / `armlink` / `fromelf`）。
安装路径不同时设置环境变量 `KEIL_ARMCLANG_BIN`。

链接区域卡死在 `0x08004000 + 0x5C000`（= `BL_APP_SIZE`），
一旦超出，`armlink` 会直接报 `L6406E` —— 不必等上板才发现。

## 升级

板子在 IAP 等待状态时（串口能看到它周期性发 `C`）：

```bash
python TestApp/tools/ymodem_send.py COM3 build/app_test.bin
```

预期输出：

```
===== LUMOS APP (test) =====
[app] running at  : 0x08004531
[app] SCB->VTOR   : 0x08004000
[app] PCLK2      : 84000 kHz
[app] CPACR      : 0x00F00000
[app] UART 115200 8N1 ready
----------------------------
[app] alive, tick=0
[app] alive, tick=1
...
```

## 作为参考实现：写一个能跑在 Bootloader 后面的 APP，必须做对这四件事

这四条都是实测踩出来的，任何一条漏掉，现象都是"APP 完全不输出"。

### 1. 重定位向量表

```c
SCB->VTOR = APP_BASE;      /* 0x08004000 */
```

必须在 `main()` 之前（`SystemInit` 里）完成。否则 APP 的中断会打到
Bootloader 的向量表上去。

### 2. 使能 FPU

```c
SCB->CPACR |= (0xFUL << 20);   /* CP10 / CP11 全访问 */
__DSB();
__ISB();
```

复位后 `CPACR = 0`（禁止访问协处理器）。用 `-mfloat-abi=hard` 编译出来的
代码只要碰到一条 VFP 指令，就会触发 `UsageFault(NOCP)` 并升级成
`HardFault` —— 而报错信息完全不会提到浮点，极难猜。
**这一步必须在任何可能用到浮点的代码之前。**

### 3. 清掉 Bootloader 遗留的 SysTick

```c
SYST_CSR = 0;
SCB_ICSR = (1UL << 25);   /* PENDSTCLR */
```

`SysTick` 属于**系统异常**，不在 NVIC 里，所以 Bootloader 那把
"清 NVIC"的循环管不到它。如果它仍处于使能状态，APP 会被周期性中断打断，
而 APP 的向量表里 `SysTick_Handler` 通常是空的（`Default_Handler`），
于是直接卡死在异常处理里 —— 同样表现为"完全不输出"。

Bootloader 侧已经修掉了根因（见 `bl_port_system_stm32f4.cpp` 的说明），
这里再清一遍是廉价的双保险。

### 4. 管好看门狗

IWDG 一旦启动就无法停止（只能靠复位），**系统复位也不会复位它**
（它在 VDD 域）。Bootloader 启用它之后，APP 必须持续喂狗：

```c
IWDG->KR = 0xAAAA;        /* 喂狗 */
```

如果要自己配置 IWDG，**顺序必须是**（与 ST 的 `HAL_IWDG_Init` 一致）：

```c
IWDG->KR = 0xCCCC;        /* 1. 先启动 —— 硬件会顺带打开 LSI */
/* 2. 等 SR 的 PVU/RVU 落（要有超时！） */
IWDG->KR = 0x5555;        /* 3. 允许改写 PR/RLR */
IWDG->PR = ...;  IWDG->RLR = ...;
/* 4. 等参数生效（要有超时！） */
IWDG->KR = 0xAAAA;        /* 5. 喂一次 */
```

反过来（先 0x5555 + 参数、再 0xCCCC）会死锁：LSI 还没起振时 IWDG
没有时钟，`SR` 的 `PVU/RVU` 会一直保持 1，任何"等它清零"的循环都出不来。

## 启动阶段标记（诊断技巧）

`app_main.c` 里在 RAM 高位（`0x20010000` 起）写了一系列阶段标记：

| 序号 | 含义 |
|---|---|
| 0 | 进入 SystemInit |
| 1 | FPU 已使能 |
| 2 | VTOR 已设 |
| 3 | 时钟配置完成 |
| 4 | 实际 PCLK2（kHz） |
| 5 | 进入 main |
| 6 | 串口配置完成 |
| 7 | IWDG 配置完成 |
| 8 | 即将输出 banner |

串口完全不出声时，用调试器读这几个字就能立刻知道 APP 执行到了哪一步，
比反复猜要快得多：

```python
# 用 pyocd
for i in range(9):
    print(hex(target.read32(0x20010000 + 4*i)))
```

## 关于 confirm（尚未实现）

当前测试 APP **不调用** Bootloader 的确认接口，所以固件会一直停在
`TESTING` 状态，启动 3 次后自动回滚（这是防变砖设计的预期行为）。
若要让它稳定驻留，需要通过双方约定的接口把自己标记为 `VALID`。
