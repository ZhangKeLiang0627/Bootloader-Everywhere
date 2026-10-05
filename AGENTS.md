# AGENTS.md — Bootloader-Everywhere 项目接手指南

> 写给接手的 AI（Agent）：**只看这一份就能理解项目、继续开发、移植新芯片。**
> 使用者文档是 `USER.md`；库的权威文档是 `Bootloader/README.md`。

---

## 1. 项目定位

串口 IAP Bootloader，做成**可移植的库**：拷 `Bootloader/` 目录进任何工程就能用。

三条硬性设计约束（用户定的，别改）：

1. **库不做初始化，也不带 `main()`** —— 时钟 / 串口 / Flash 接口时钟全由宿主工程负责，
   宿主在 `main()` 里初始化完，在 `while` **之前**调一次 `blRun()`（它不返回）
2. **不用看门狗、不做自动回滚** —— IAP 是人站在板子前刷的，不是 OTA，这些机制成本大于收益
3. **不靠 Flash 存状态** —— 提交靠「向量表最后写」（见 §3），因此没有配置区

---

## 2. 仓库结构

```
README.md              入口导航（"从哪开始"）
AGENTS.md / USER.md    给 AI / 给使用者
Bootloader/            ★ 库本体，7 个源文件，0 子目录
  README.md              库的唯一文档（提交机制 / 用法 / 移植 / 常见坑）
  bl.h                   对外头文件：blRun() + blUartRx() + Status（C 工程也能 include）
  bl.cpp                 主体：日志 / CRC32 / 向量表校验 / IAP 命令 / 决策 / 入口
  protocol.h             载体层：Frame / Parser / encode / crc8（与业务无关，可整对拷走）
  protocol.cpp           载体层实现：0xA5 帧编解码 + CRC8
  bl_port.h              移植契约（12 个函数 + 扇区表）
  bl_port_stm32f4.cpp    STM32F4 实现（含板级配置：串口实例 / 引脚）
  bl_config.h            只需填 2 个数：BL_FLASH_SIZE / BL_BOOT_SIZE
tools/                 开发工具（不属于库）
  build.py               命令行编译 + 量 ROM（armclang / armlink / fromelf）
  board.py               pyocd 板端操作（备份/烧写/擦除/看串口）
Core/ Drivers/ MDK-ARM/ Bootloader-Everywhere.ioc
                       STM32F401 示例工程（不是库的一部分）
UserApp/main.cpp       本工程自己的代码入口（见 §2.1）
docs/                  设计文档
  PROTOCOL_DESIGN.md     协议设计：§0 是现行 0xA5 帧协议的规格与实现要点
  PERF_COMPARISON.md     性能对比：与 YMODEM / esptool / mcumgr / OpenBLT / UDS 的
                         帧开销、端到端耗时与能力对比，含优化清单
TestApp/               （仅 test-app 分支）测试 APP + 板端测试脚本
build/                 编译产物
```

**库与示例的分界**：想用库只需拷 `Bootloader/`。示例工程只是"怎么接进真实工程"的
演示 —— **是库去配合别人的工程，不是别人配合这个仓库**（用户明确要求）。

---

### 2.1 示例工程的宿主结构（重要：别往 CubeMX 的文件里塞代码）

```c
/* Core/Src/main.c —— CubeMX 生成，只有 USER CODE 区那行是我们要的 */
int main(void)
{
    HAL_Init();
    SystemClock_Config();      /* HSI+PLL -> 84MHz（本板 HSE 晶振起不来） */
    MX_GPIO_Init();
    MX_USART1_UART_Init();

    Main();                    /* <- USER CODE 区，唯一属于我们的调用 */

    while (1) { }
}
```

```cpp
/* UserApp/main.cpp —— 我们自己的代码都放这儿，重新用 CubeMX 生成不会丢 */
extern "C" void Main(void)
{
    bootPinInit();             /* PC0：按住上电 = 留在 IAP */
    bootLedInit();             /* PC13：BL 运行期间闪烁 */
    for (;;) { blRun(); }      /* 不返回：要么在 IAP 里等，要么跳 APP */
}
```

要点：
- `Core/` 下所有文件（main.c / gpio.c / usart.c / stm32f4xx_it.c / …）都是 CubeMX 的，
  改动**只允许**在 `USER CODE BEGIN/END` 区里（比如 SysTick_Handler 里的 LED 翻转）
- 时钟用的 HSI 而不是 HSE：本板晶振起不来（实测 HSERDY 恒 0）；换板子要同步改
  `stm32f4xx_hal_conf.h` 的 `HSE_VALUE` 与这段配置
- `blRun()` 放在 `for(;;)` 里只是语义清楚 —— 它不返回，循环体实际只执行一次

---

## 3. 核心机制（改动前必须理解）

### 3.1 提交 = 写 APP 区的前两个字

`Session` 接收时把 APP 区最前 8 字节（SP、PC）扣在 RAM（`entry_[8]`）不写；
其余数据照常写。整份收完后先 **回读 `[8, size)` 重算 CRC32 比对**，
通过才写回 SP、PC —— 这一步就是提交点。

上电判据只有一条：**向量表的 SP/PC 合法吗**（`vectorsSane()`：SP 落在 SRAM 内、
PC 落在 APP 区内且最低位为 1）。由于这两个字是最后写的，
"合法" 就等价于"整份固件已完整写入并校验过"。

于是**任何时刻断电都停在 IAP**：SP/PC 要么还是擦除态 `0xFFFFFFFF`，要么只写了一半。
不需要配置区、状态机、魔术数。

> ⚠️ 曾经的设计（已删）在 APP 区末尾之外单独占一个 128KB 配置扇区存 `{state, size, crc32}`
> 并做「日志式槽位轮转」，里面有 `Meta` 类与 `kSlotMagic = 0x4C554D53 ("LUMS")`。
> 用户问"为什么还要存魔术数"之后被这套机制取代。**不要把它加回来。**

### 3.2 决策 4 步（`Boot::decide()`）

```
1. 按住硬件按钮上电     → EnterIap     （无限等：人就在旁边，零等待）
2. 向量表非法          → EnterIap     （空片 / 传输没提交，停在 IAP 可重刷）
3. 复位原因 = 软件复位  → EnterIapTimed（15s 限时窗口，APP 唤回的通道）
4. 其余                → JumpToApp     （零等待）
```

**第 1 步是保留功能，和已删的「串口 backdoor」不是一回事**（见 §7）：它读的是
「按住」的电平，不需要抢时间窗，也不拖慢正常启动。引脚/极性在
`bl_port_stm32f4.cpp` 顶部四行宏里配；引脚本身由宿主配成「输入 + 上拉」
（示例见 `Core/Src/main.c` 的 `bootPinInit()`）。板子上没按钮就把那四行注释掉。

`resetCause()` 必须每次启动都读（read-and-clear）；实现里 **`SFTRSTF` 优先级高于
`POR/PIN`** —— 在线探针连着时软件复位会连带把 NRST 拉一下，两者同时置位。

### 3.3 APP 唤回

APP 侧不需要库提供任何接口：检测到关键字（默认 `#Bootloader-Everywhere`）后
**做一次软件复位**即可，Bootloader 靠复位原因识别。示例见 `USER.md`。

---

## 4. 构建与验证

### 4.1 Keil（AC6）

```
UV4 -r MDK-ARM/Bootloader-Everywhere.uvprojx -j0 -o build.log    # 全量重建
UV4 -f MDK-ARM/Bootloader-Everywhere.uvprojx -j0 -o flash.log    # 编译 + 下载
```

> ⚠️ **`-f` 不保证重编改动过的文件**。改过源文件后验证必须用 `-r`，
> 否则会看到旧代码的行为（本项目为此白排查过三轮"时钟没配上"）。

C++ 标准在 Keil 的下拉框里已设为 C++11（`v6LangP=3`），**不要往 MiscControls
里加 `-std=`** —— 那些参数会同样作用到 C 文件上。

### 4.2 命令行量 ROM

```bash
python tools/build.py            # 默认 F401，-Oz + 日志
python tools/build.py --no-log   # 关日志，看发布版体积
python tools/build.py --all      # 各芯片 × 各优化等级对比
```

`build.py` 会注入一个最小宿主（`SystemClock_Config` + `HAL_MspInit` + `main`），
使量出的数字是**库本体**，不把宿主的 HAL 时钟代码算进来。

### 4.3 真板回归（test-app 分支）

```bash
python TestApp/build_app.py --fail 0                      # 正常固件 app_test.bin
python TestApp/tools/test_proto.py                        # 正常路径 T1-T5
python TestApp/tools/test_proto_edge.py                   # 边界与畸形输入 E1-E15
python TestApp/tools/test_proto_perf.py --sizes 2,64,200  # 耗时实测（KB）
```

测试项：
- T1 正常升级并跳转 / T2 连续升级 x5 / T3 传输中断不变砖 /
  T4 篡改帧被拒且可恢复 / T5 跳号被拒并给出续传点
- E1-E15 边界与畸形输入（超大固件 / 极小固件 / 向量表非法 / 各类字段不一致 /
  载体层丢帧 / 坏固件被提交后仍能恢复），**每条都断言被拒绝时 APP 区未被改动**
- 实测耗时与优化空间见 `docs/PROTOCOL_DESIGN.md` §0.7

**库改动后必须上板跑一遍**，不能只靠编译通过。

---

## 5. 板子与工具环境

| 项 | 值 |
|---|---|
| 板子 | STM32F401RET6（512KB / 96KB），DEV_ID `0x433`，APP 区 `0x08004000` + 496KB |
| 串口 | COM3 @ 115200 8N1（USART1 PA9/PA10），独立 Type-C 5V 供电 |
| 探针 | DAPLink（CMSIS-DAP）。**pyocd 烧写不稳定**（`result code 0x1` / `Unexpected ACK '6'`），优先用 `UV4 -f` |
| pyocd | 在隔离环境 `~/.workbuddy/binaries/python/envs/default`，目标名 `stm32f401retx` |
| ⚠️ 本板 HSE | **晶振起不来**（HSERDY 恒 0，HSEBYP 下才置位）→ 示例工程用 HSI+PLL → 84MHz |

---

## 6. 移植新芯片

写 `bl_port.h` 的 12 个函数 + 一张扇区表：

```
flashErase  flashWrite  flashRead
uartRead  uartWrite  uartFlushRx  uartRxIrqHandler
tickMs  delayMs
resetCause  jumpToApp
bootPinHeld        没有按钮的平台直接 return false;

kFlashSectors[]    扇区表 {base, size}；查表用 bl_port.h 的 flashSectorAt()
```

要点：

- `bl.cpp` 一个字都不改；板级差异（用哪路串口、哪几个引脚）留在
  `bl_port_<平台>.cpp` 顶部的配置区
- `flashErase` 必须拒绝擦除 Bootloader 自身区域
- 扇区可能不等长（F4：0-64KB 每扇区 16KB、64-128KB 一扇区 64KB、之后每扇区 128KB），
  所以擦除按 `flashSectorAt()` 查表逐个走（表由 port 给，算法在 bl.cpp）
- 迁移时改 `bl_config.h` 的 2 个数：`BL_FLASH_SIZE` / `BL_BOOT_SIZE`
  —— SRAM 不用配（判据只做「像不像栈顶」的宽检查，见 §4.x `vectorsSane()`）
- 没有复位原因寄存器的芯片：`resetCause()` 返回 `Unknown`，那就用不了唤回窗口
  （其余功能正常）
- port 用 HAL 收发串口（`HAL_UART_Receive/Transmit`）：需要宿主的句柄名，
  板级配置区一行 `extern "C" UART_HandleTypeDef huart1;`。
  换芯片时这一行也在 port 文件里改，`bl.cpp` 不用动

---

## 7. 刻意删掉的机制（不要加回来）

| 已删除 | 原因 |
|---|---|
| 看门狗（IWDG）+ 自确认回滚 | 用户明确要求：IAP 是人在旁边刷的，放弃防回滚 |
| 配置区 / `Meta` 类 / `kSlotMagic` / 槽位轮转 | 被「向量表最后写」取代（§3.1） |
| YMODEM（第三方工具刷机的便利性） | 已整体删除，换成自定义 0xA5 帧。**别因为"通用工具能刷"再引入** —— 那正是换掉的代价 |
| 串口 backdoor（上电 300ms 内按 DEL 进 IAP） | 鸡肋：拖慢每次启动，正常人也卡不准。**别和「按住硬件按钮上电」搞混 —— 那个是保留功能（§3.2），一起删掉就少了一条救命通道** |
| RAM 标志（APP 写 magic 后软复位） | 被「纯复位原因」取代，APP 侧零侵入 |
| 库自带 `main()` / `chipInit` / `flashInit` / `uartInit` | 库不初始化芯片、不带 main（§1） |
| `uartTryGetc` / `uartsSetBaudrate` / `verifyImage` / `VectorCheck` | 死代码 |
| `blRequestUpdate` / `BL_BOOT_MAGIC_STRING`（库内） | APP 侧接口不进库，示例放 `USER.md` |
| `FwState` / `IapResult::NoSpace` / `ResetCause::BrownOut,LowPower` | 不再使用 |
| 文件头大块注释 + 三行分节 banner | 用户要求：改成单行 `//` 标题 |
| 启动时整镜像 CRC（需存 size/crc32） | 用户明确决定：**只在烧录末尾回读校验**就够，不为它保留存储（漏掉的只是刷完之后才发生的 Flash 位翻转） |

---

## 8. 常见坑（工程与流程类）

1. **GPIO 端口时钟没开时读 IDR 恒为 0（本板实测）**：用它做「按下」判据会
   每次都误判 → 上电永远进 IAP、APP 起不来。`bootPinHeld()` 因此先查时钟位，
   没开就按「没按」处理
2. **`UV4 -f` 不重编**：改过源文件必须 `-r`（§4.1）
3. **切分支后工作区文件大面积显示 `D`**：`git checkout HEAD -- <路径>` 恢复。
   实测不止 `Bootloader/`，`docs/`、`tools/` 也会被清（2026-10-06 又踩到两次）。
4. **写完新文件立即 `git add`**：工作区被清时**未入库的文件找不回来** ——
   `docs/js/protocol.js` 等 4 个文件就这样丢过一次，只能照记忆重写。
   另外：`git checkout -- <path>` 恢复之后**不要再用写文件的工具碰同一目录树**，
   恢复的文件会被再清一次（本项目连续踩到）。
5. **切分支后不要盲发 `git add -A`**：会把「工作区被清」这件事当成删除一起提交 ——
   `docs/` 被清那次就这么误删了 `docs/PROTOCOL_DESIGN.md`。
   提交前先看一遍 `git status`，`D` 开头的都要先确认是不是被清的。
6. **同一消息里批量 Edit 同一文件会静默丢部分改动**：改完 grep/Read 复核
7. **轮询模型下别发多字节命令**：YMODEM 之外的自定义交互要按字节慢发
8. **`%lu` 依赖 `%u` 分支**：精简日志格式化时删 `%u` 会让所有 `%lu` 打成字面量 `%u`
   （真板实测暴露过）
9. **`HSE_VALUE` 真相源只有一个**：`stm32f4xx_hal_conf.h`；别在 Keil 的 `<Define>`
   里再定义一次
10. **`bl.h` 要能被 C 包含**：C++ 部分（`namespace bl`）必须在 `#ifdef __cplusplus` 里，
   且用 `<stdint.h>` 而不是 `<cstdint>`
11. **`target.write_memory_block8()` 改不了 Flash**：实测对本芯片的 Flash 地址
   **完全不生效**（写完读回内容不变）—— Flash 只能 1→0，未经擦除写不进去，而
   pyocd 的内存写路径也不会自动走 Flash 编程算法。改 Flash 只有两条路：擦扇区
   （`FlashEraser`）或烧 hex/bin（`FileProgrammer`）。
   本项目因此踩过一个**测试假阳性**：用「写 `0xFF`」来清向量表其实是空操作，而设备
   被探针复位带进了 15 秒窗口、STATUS 有应答，于是测试误判成「已恢复」
12. **探针复位会置 `SFTRSTF`**：`reset_and_halt()` 让设备被判成「软件复位」→ 进 15 秒
   唤回窗口。所以「复位后是否停 IAP」不能只看 STATUS 有没有应答（窗口期内也有），
   要抓串口启动日志看 `decision: ...` 那一行来区分

---

## 9. commit 约定

```
@操作(关键词): 描述

Co-Authored-By: Claude <noreply@anthropic.com>
```

`@` 后接 `update` / `add` / `fix` / `refactor` / `feat` / `delete` 等；
署名行前空一行。git 身份用 `kkl / 1184665829@qq.com`。

分支改动要一起同步（`main` / `test-app` / `web`），文档改动尤其别漏。
新协议在 `protocol-v2` 上开发，网页适配在 `web-v2` —— **两套协议互不兼容**，
别把协议相关的改动混着同步过去。

---

## 10. 传输协议：已换成自定义 0xA5 帧（2026-10-06，分支 protocol-v2）

设计见 **`docs/PROTOCOL_DESIGN.md`**（§0 是现行规格，第一至三章为调研记录）。要点：

- **载体层 = `protocol.{h,cpp}`**：`A5 | ID | CMD | len:2 LE | Data≤1024 | CRC8 | 03`。
  只依赖 `stdint.h`，与芯片、业务无关 —— **可以整对拷到 APP / 上位机 / 别的工程复用**。
  `CMD` 的 bit7 = 方向位；CRC8（SMBUS，MSB-first）覆盖 `[ID..Data]`，不含头尾。
- **IAP 命令层在 `bl.cpp`**：`START`(size/crc32/sp/pc) → `DATA`(addr/total/index/vlen/cumCrc32/data)
  → `END`。16 个错误码见设计文档 §0.5.7。
- **DATA 一应一答**；**先校验后写入**（Flash 只能 1→0，先写坏就要整扇区擦除才能纠正）；
  写后**读回 Flash 重算 CRC32**（只对收到的字节累加与主机算的必然相同，没有信息量）。
- **帧长自检**：帧长必须落在 `[proto::kFrameMin, proto::kFrameMax]`（7 - 1031）。
  `Parser` 与 `readFrame()` 都会校验，长度可疑的帧直接丢、让主机重传。
- 传输层**中断接收**：`uartRxIrqHandler` + 512 B 环形缓冲，寄存器实现，不用 HAL_UART。
  宿主只需在 `USART1_IRQHandler` 里调 `blUartRx()`，**不要**再调 `HAL_UART_IRQHandler`。
- 上位机 `TestApp/tools/proto.py`；板端测试四个脚本：`test_proto.py`（T1-T5 正常路径）、
  `test_proto_edge.py`（E1-E15 边界与畸形输入）、`test_proto_perf.py`（耗时实测）、
  `stress_iap.py`（S1-S13 压测：连续升级 / 逐帧错误注入 / 应答丢失幂等 /
  重放污染探测 / 跳号续传 / 背靠背会话 / 突发帧 / 空闲超时 / END 整片回读校验）。
  载体层另有 PC 侧压测 `tools/stress_protocol.cpp`（fuzz + 突变 + 恢复能力）。
- 性能对比（与 YMODEM / esptool / mcumgr / OpenBLT / UDS）见 `docs/PERF_COMPARISON.md`：
  纯协议效率不是瓶颈（换成最省的 YMODEM 也只快 1.2 秒/200KB），
  **波特率是唯一的数量级杠杆**（实测 3.5-4.3 倍），块大小在高速下才重要。

> ⚠️ **不要再引入 YMODEM**（实现已整体删除）。"任何第三方工具都能刷"这个便利性是有意
> 放弃的 —— 换来了可读的帧格式、精确的错误定位与可扩展的命令空间，代价见设计文档「代价与风险」。

---

## 11. 分支拓扑与合并状态（2026-10-06）

```
main          库 + STM32F401 示例工程（YMODEM 协议）
test-app      库 + TestApp/ 测试 APP 与脚本（YMODEM）—— 上板验证都在这个分支做
web           docs/ 网页上位机（YMODEM），GitHub Pages 从这里发布
protocol-v2   ★ 新协议（0xA5 帧）：载体层 + IAP 命令层 + 中断接收，真板 T1-T5 已过
web-v2        从 web 拉出，把网页适配到新协议（进行中）
```

**两套协议互不兼容**：`protocol-v2` 的固件只能被 `proto.py` / `web-v2` 刷，
`main` 等分支的固件只能被旧网页刷。别把协议改动直接同步过去。

- `protocol-v2` 合回 `main` / `test-app` 之前，先确认 `web-v2` 能用（否则线上刷机页会失效）
- 协议层改动要同步三处实现：固件 `protocol.cpp`、上位机 `proto.py`、网页 `web-v2` ——
  三处的 `selftest` 必须给出相同的向量结果
