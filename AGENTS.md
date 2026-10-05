# AGENTS.md — LUMOS-bootloader 项目接手指南

> 写给接手的 AI（Agent）：**只看这一份就能理解项目、继续开发、移植新芯片。**
> 使用者文档是 `README.md`（仓库根）；库的权威文档是 `Bootloader/README.md`。

---

## 1. 仓库结构

```
README.md              入口导航（"从哪开始"）
AGENTS.md / USER.md    给 AI / 给使用者
Bootloader/            ★ 库本体，5 个文件，0 子目录
  README.md              库的唯一文档（含执行流程、移植指南、常见坑）
  bl.h                   唯一入口（blRun / blRequestUpdate / 公共类型）
  bl.cpp                 全部实现（日志/CRC/元数据/校验/YMODEM/会话/决策/入口）
  bl_port.h              移植契约（15 个函数 + 每条的实现要点）
  bl_port_stm32f4.cpp    STM32F4 实现（含板级配置：串口/引脚）
  bl_config.h            编译期配置（Flash 分区 / 通信 / 日志开关）
tools/                 开发工具（不属于库）
  build.py               命令行编译 + 量 ROM（armclang）
  board.py               pyocd 板端操作（备份/烧写/擦除/看串口）
Core/ Drivers/ Bsp/ UserApp/ MDK-ARM/ LUMOS-bootloader.ioc
                       STM32F401 示例工程（CubeMX + Keil），非库的一部分
build/                 编译产物
```

**库与示例的分界**：想用库只需拷 `Bootloader/`。示例工程只是"怎么接进真实工程"
的演示 —— **是库去配合别人的工程，不是别人配合这个仓库**（用户明确要求）。

---

## 2. 三个分支

| 分支 | 内容 |
|---|---|
| `main` | 库 + STM32F401 示例工程 |
| `test-app` | 库 + `TestApp/`（测试 APP、测试脚本） |
| `web` | `docs/`（网页上位机），push 后 Pages 自动重建 |

改库的流程：**main 改 → 提交 → 切 test-app 同步 `git checkout main -- Bootloader/`
→ 上板验证 → 提交 test-app**。改文档记得三分支都同步。

> ⚠️ **切分支后 `Bootloader/` 的文件可能大面积丢失（显示 `D`）**，这个仓库反复
> 出现过。切完分支第一件事就是 `git status`，丢了就 `git checkout HEAD -- Bootloader/`。

---

## 3. 库内部分工（改了代码放哪）

| 想改什么 | 改哪 |
|---|---|
| Flash 分区 / 超时 / 波特率 | `bl_config.h` |
| 串口用哪一路、哪几个脚、板级时钟 | `bl_port_stm32f4.cpp` 顶部「板级配置」 |
| 协议、升级流程、决策逻辑 | `bl.cpp` |
| 换芯片 | 照 `bl_port_stm32f4.cpp` 再写一份；`bl.cpp` 一个字不改 |
| 系统时钟 | **都不在这几个文件里** —— 库调用宿主的 `SystemClock_Config()`（见 §5.5） |

`bl.cpp` 的章节顺序就是"上电后会发生什么"：①日志 ②CRC ③元数据 ④校验
⑤YMODEM ⑥会话 ⑦决策 ⑧入口。找东西按这个顺序翻。

**铁律**：`bl.cpp` 里**不得出现任何芯片厂商头文件**。可自动验证：

```bash
grep -E '#include *[<"](stm32|gd32|ch32|hal)' Bootloader/bl.cpp   # 必须无输出
```

`bl.cpp` 也**不得出现任何时钟/频率数字** —— 那是宿主的事。

---

## 4. 核心机制

细节与代码见 `Bootloader/README.md` 的「执行流程」一节，这里只记要点。

### 4.1 启动决策（`bl.cpp` ⑦ 决策节）

```
① 固件状态可跳转？       否（Invalid/Download）→ 进 IAP，无限等
② 软件复位？             是 → 进「15s 限时窗口」（APP 唤回主通道）
③ 向量表合法？           否 → 进 IAP
④ 整镜像 CRC32 匹配？    否 → 进 IAP
⑤ 全通过                 → 跳 APP
```

`resetCause()` **每次启动必读**（read-and-clear）：复位标志是累积的，只有读才清。
漏读会让标志粘住，下次启动误判。

### 4.2 升级的原子提交（`bl.cpp` ⑥ 会话节）

```
① meta.markDownload()  置 Download（先让固件"不可信"）
② 擦除 APP 区
③ 接收 YMODEM 并写入
④ 边收边算 CRC32
⑤ meta.commit()         置 Valid + 记录 size/crc32/版本 → 直接跳转
```

①必须在②之前。顺序反了，擦除中途掉电会留下"状态可跳转但 APP 是空片"的必砖组合。

### 4.3 唤回窗口

- 握手超时 = `BL_YMODEM_HANDSHAKE_MS`（15s）；置 0 = 无限等
- 窗口内没等到首包 → 跳回 APP（判据：`Failed + Timeout + fwSize == 0`）
- 正常上电/按复位 → 零等待直接跳 APP

### 4.4 配置区（`bl.cpp` ③ 元数据节）

**单槽原地擦写**：状态变更就「擦整片配置区 → 写一条 64B 记录」。不做槽位轮转、
不记序号。代价是每次状态变更等一次扇区擦除（约 1 秒），换来的是代码少一半、
且天然没有"残槽"问题（擦除途中掉电只会变全 FF = Invalid）。

### 4.5 系统时钟由宿主提供

库**一行时钟配置都没有**，`chipInit()` 里只有一句 `SystemClock_Config();`
（强引用，宿主不提供就链接报错）。示例工程的实现在 `Core/Src/bl_clock.c`。

> ⚠️ **本板 HSE 晶振起不来**（实测 `HSERDY` 恒 0，只有 HSEBYP 旁路下才置位），
> 所以示例工程用 HSI+PLL：16MHz / PLLM16 × PLLN336 / PLLP4 = 84MHz。
> 旧的库内实现在 HSE 失败时会静默退回 HSI 16MHz，加上 `[clk]` 日志打在串口
> 就绪之前一直是丢的 —— 这件事很久没人发现。现在 `[clk] sysclk=` 打在
> `uartInit()` 之后，开机就能看见。

---

## 5. ⚠️ 刻意删掉的机制（不要加回来）

2026-10-05 按用户要求整体删除，**本仓库不应再出现**：

| 已删除 | 原因 |
|---|---|
| 看门狗 IWDG（`BL_USE_WATCHDOG`/`wdgInit`/`wdgFeed`） | 一旦启动无法停止，会强迫所有 APP 必须喂狗；对 IAP 是纯负担 |
| 自确认 + 回滚（`BL_BOOT_SELF_CONFIRM`/`MAX_ATTEMPTS`） | 依赖看门狗复位作判据；IAP 是人在场操作，不需要自动回滚 |
| `FwState::Testing`/`Revoked`、`bootAttempts`、`confirmApp()` | 上面两项的配套状态 |
| Backdoor 时间窗（`waitBackdoor`）、`uartTryGetc()` | 已被「软件复位唤回 + 15s 窗口」取代，后者成了孤儿 |
| 配置区请求升级（`requestUpdate`/`kFlagUpdateReq`） | 无调用点，被关键字唤回取代 |
| `uartSetBaudrate`/`consoleUart`/`systemReset`/`IapResult::Running` | 预留但从未使用的死代码 |
| **库内的时钟配置**（8 个时钟宏 + 芯片预置 + HAL 枚举翻译层） | 时钟是宿主工程的事（见 §4.5） |
| `BL_PROVIDE_MAIN` 开关 | 库固定自带 main |
| `ResetCause::BrownOut`/`LowPower` | `decide()` 从不判定 |

删掉看门狗后，**APP 不需要也不能喂狗**；坏固件刷进去会卡住，人工重上电即可。

---

## 6. 构建与验证

### 6.1 Keil（编译 + 下载，最常用）

工程：`MDK-ARM/LUMOS-bootloader.uvprojx`；UV4：`C:\Users\11846\AppData\Local\Keil_v5\UV4\UV4.exe`

```bat
UV4 -r proj.uvprojx -j0 -o build.log   :: 全量重建
UV4 -f proj.uvprojx -j0 -o flash.log   :: 下载（烧到板子）
```

退出码 0=无错无警告，1=有警告，2=有错误。日志搜 `error:`。

> ⚠️⚠️ **`UV4 -f` 不保证重编改动过的文件**。本项目为此浪费过三轮调试：
> 改了 `bl_port_stm32f4.cpp` 后 `-f` 仍用旧 `.o`，反汇编里还是旧代码。
> **凡是改了源文件，验证前必须 `UV4 -r` 全量重建。**

语言标准：**C++11**（Keil → Options → C/C++ → Language C++，对应 `<v6LangP>3</v6LangP>`）。
**不要往 MiscControls 里加 `-std=c++11`** —— 那份设置会同时作用到 C 文件上，
armclang 会报 `not allowed with 'C'`。

### 6.2 命令行量 ROM

```bash
python tools/build.py            # 量 ROM
```

依赖 Keil AC6 工具链（armclang/armasm/armlink/fromelf），可用 `KEIL_ARMCLANG_BIN` 覆盖。
当前基准：库本体 12560 B；加上示例工程 `bl_clock.c` 后 13848 B。

### 6.3 测试（test-app 分支）

```bash
python TestApp/build_app.py --fail 0   # 正常固件
python TestApp/build_app.py --fail 1   # 挂死固件
python TestApp/build_app.py --fail 2   # HardFault 固件
python TestApp/tools/test_auto.py      # 一键 T1-T5，逐项判 PASS/FAIL
python TestApp/tools/power_test.py --phase N   # 断电暴力测试（人工拔电配合）
```

当前 T1-T5 真板全通过（F401RET6）：正常升级 / 连续升级 x5 / 软件复位唤回 /
窗口超时跳回 APP / 传输中断不变砖。

### 6.4 板端环境

- 编译：Keil MDK（本地 Windows）
- 烧写：DAPLink（CMSIS-DAP，`0d28:0204`）。⚠️ **pyocd 烧写经常 `result code 0x1`
  （线缆信号完整性），优先用 `UV4 -f`**（Keil 自带驱动绕过该问题）
- 串口：CH340，COM3 @115200 8N1；供电：独立 Type-C 5V
- 探针直读寄存器验签是可靠手段（例：读 `RCC_CFGR`/`RCC_PLLCFGR` 反推实际时钟）

---

## 7. 移植新芯片

**先读 `Bootloader/README.md` 的「移植到别的芯片」，那里是权威。**

要点：
- 只改 `bl_config.h` 的 4 个分区数字 + 写自己平台的 `bl_port_*.cpp`
  （同族 F401/F405/F407 之间通常只需改分区）
- 差异集中在擦除单位（F4 不等长扇区 vs 多数国产芯片等长页）、
  编程单位（32 vs 16 位）、串口寄存器
- **RISC-V 内核（CH32V307）**：向量表不是 `SCB->VTOR`，跳转流程要查手册
- 时钟不用管 —— 宿主工程自己配

---

## 8. 常见坑（工程/流程类，都实测过）

1. **`UV4 -f` 不重编**：见 §6.1，验证前必须 `-r`
2. **探针拖 PIN 复位**：DAPLink 连着时软复位会连带置 `PINRSTF`，
   `resetCause()` 必须让 SFTRSTF 优先，否则唤回在开发期失灵
3. **pyocd 烧写 `0x1`**：优先 `UV4 -f`
4. **切分支后 `Bootloader/` 文件丢失**：`git checkout HEAD -- Bootloader/`
5. **批量 Edit 部分丢失**：同一消息里对同一文件发多个 Edit 可能静默丢，改完务必验证落盘
6. **别用 `-std=` 覆盖 C 语言**：见 §6.1
7. **测试脚本别盲等单字节**：bootloader 会周期性补发 `C`，按日志文本判定更稳
8. **弱符号钩子会被内联掉**：用「弱默认空实现」当扩展点时，armclang 可能把空
   实现内联、导致宿主的强定义无人引用而被回收。要么用强引用，要么让宿主自己调

（库运行期的坑见 `Bootloader/README.md` 的「常见坑」）

---

## 9. commit 约定

- 作者固定：`kkl <1184665829@qq.com>`
- 格式：
  ```
  @操作(关键词): 详细修改信息

  Co-Authored-By: Claude <noreply@anthropic.com>
  ```
  `@` 后接 `update`/`add`/`delete`/`fix`/`refactor`/`feat` 等，署名行前空一行。

---

## 10. 接手后的操作清单

1. `git status` + `git branch -a`；若 `Bootloader/` 有文件丢失先恢复
2. 明确任务属于哪个分支（库→main，测试→test-app，网页→web）
3. 改完编译：库用 `UV4 -r` 或 `tools/build.py`；网页用 `node --check docs/js/app.js`
4. **库改动必须上板**（`UV4 -r` → `UV4 -f` → COM3 观察 → `test_auto.py`），光编译不算完
5. 提交按 §9，推送对应分支；文档变更三分支同步
6. 每次迭代后更新 README（文档是交付的一部分）
7. **大改动先提交再删旧文件** —— 本项目踩过一次"新旧交替时误删未提交的新文件"
