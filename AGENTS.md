# AGENTS.md — LUMOS-bootloader 项目接手指南

> 本文档写给接手的 AI（Agent）。目标：**只看这一份就能理解项目、继续开发、移植新芯片。**
> 配套的 `Bootloader/docs/PORTING.md` 是移植细节的权威文档，本文给出总览并指向它。

---

## 1. 这是什么

**LUMOS-bootloader** 是一个可移植的串口 IAP Bootloader：

- 通过 YMODEM-1K 串口协议刷固件，写入 APP 区 → 校验 → 跳转
- **软件复位唤回**：APP 运行中收到关键字 `#Bootloader-Everywhere` 即软复位，
  Bootloader 靠复位原因识别并开一个 15s 限时升级窗口
- 提供网页版上位机（Web Serial），有网 + Chrome 就能刷机，无需装任何软件
- 零依赖、跨芯片：同一份 `Bootloader/` 目录拷到任何 Cortex-M 工程都能用

**明确的设计取向（2026-10-05 起）**：**不做自动回滚、不用看门狗**。
IAP 是人站在板子前面刷的，不是 OTA；刷进坏固件就重新刷一次，不为此
增加复杂度与运行期副作用（这是与用户确认后的决策，不要再"好心"加回来）。

GitHub 仓库：`ZhangKeLiang0627/Bootloader-Everywhere`（public）
网页地址：**https://zhangkeliang0627.github.io/Bootloader-Everywhere/**

---

## 2. 仓库的三个分支（重要，接手先搞清楚在哪个分支）

| 分支 | 内容 | 什么时候用 |
|---|---|---|
| `main` | Bootloader 库 + CubeMX 宿主工程（Core/Drivers/Bsp/UserApp）+ MDK-ARM | 改 Bootloader 库本体、移植 |
| `test-app` | 库 + `TestApp/`（测试 APP、测试工具脚本） | 上板验证、跑自动化测试 |
| `web` | `docs/` 网页（index.html / css / js） | 改网页上位机，push 后 GitHub Pages 自动重建 |

**分支关系**：`test-app` 是 `main` 的派生（多了测试），`web` 是独立的网页分支。
改 Bootloader 库的流程：**在 `main` 改 → 提交 → 切 `test-app` 用 `git checkout main -- Bootloader/` 同步 → 上板验证 → 提交 test-app**。

> ⚠️ **切分支后 `Bootloader/` 目录文件会大面积丢失（显示为 `D`）**，这是这个仓库反复出现的现象（可能与 UV4 编译的副作用有关）。修复：`git checkout HEAD -- Bootloader/`。切完分支务必 `git status` 确认。

---

## 3. 目录结构（main 分支）

```
LUMOS-bootloader/
├── Bootloader/                 # ★ 可搬走的库（唯一要理解的部分）
│   ├── bl_config.h             # ★ 唯一配置面：芯片/分区/通信，移植只改这里
│   ├── bl_app.h                # APP 侧最小接口（软复位唤回 + 关键字常量）
│   ├── bl.hpp                  # 库统一入口头文件
│   ├── app/bl_entry.cpp        # 主流程入口（平台初始化→决策→IAP 循环）
│   ├── core/                   # 平台无关核心（一行不碰芯片寄存器/HAL）
│   │   ├── bl_boot.cpp/hpp     #   启动决策 decide()
│   │   ├── bl_meta.cpp/hpp     #   配置区（槽位/状态/commit）
│   │   ├── bl_session.cpp/hpp  #   升级会话（擦除→写→commit 原子化）
│   │   ├── bl_ymodem.cpp/hpp   #   YMODEM-1K 接收端
│   │   ├── bl_verify.cpp/hpp   #   向量表/整镜像 CRC 校验
│   │   ├── bl_crc.cpp/hpp      #   CRC16/XMODEM + CRC32
│   │   ├── bl_log.cpp/hpp      #   精简日志（禁用 printf，省 ROM）
│   │   └── bl_types.hpp        #   Status/IapResult/FwState/ResetCause 枚举
│   ├── port/                   # ★ 移植层接口（唯一分界线）
│   │   ├── bl_port.hpp         #   16 个抽象函数（flash×5 / uart×4 / 跳转 / 时基 / 复位原因…）
│   │   └── bl_port_template.cpp#   移植模板（复制到 target/ 后逐项填）
│   ├── target/stm32f4/         # STM32F4 具体实现（4 个 cpp + config）
│   ├── presets/                # 芯片预设（纯数字，无代码）
│   │   ├── stm32f401xe.h / stm32f405xx.h / stm32f407xx.h / gd32f303re.h
│   ├── docs/
│   │   ├── PORTING.md          # ★ 移植指南（权威）
│   │   ├── TEST_PLAN.md        # 失效模式清单（含断电暴力测试）
│   │   └── REUSE_AND_PLAN.md
│   └── tools/
│       ├── build.py            # 命令行编译 + 量 ROM + 导出 bin/hex（armclang）
│       ├── board.py            # 板端工具（backup/flash/erase/monitor）
│       └── flash_blob/         # RAM blob 烧写方案（pyocd 失败时的兜底）
├── Core/ Drivers/ Bsp/ UserApp/  # CubeMX 宿主工程（STM32F401）
├── LUMOS-bootloader.ioc          # CubeMX 工程
├── MDK-ARM/                      # Keil 工程（编译 + 下载）
└── build/                        # 编译产物
```

**分层铁律**（守不住库就废了）：
1. `core/` 不得包含任何芯片头文件，只调 `port/bl_port.hpp` 的 16 个函数
2. `core/` 不得假设 Flash 按扇区擦，只调 `flash_erase(addr,len)` / `flash_sector_size(addr)`
3. `app/` 不得直接调厂商库，延时用 `bl::delay_ms()`

---

## 4. Flash 分区（自动派生，改容量即改布局）

`bl_config.h` 第二节，三块区域地址全部由 `BL_FLASH_BASE/SIZE` 派生：

| 区域 | F401（512KB） | F405（1MB） | 作用 |
|---|---|---|---|
| Bootloader | 16KB @0x08000000 | 同 | 常驻，永不擦自己 |
| APP | 368KB @0x08004000 | 880KB | 被升级的固件 |
| 配置区 META | 128KB @0x08060000 | @0x080E0000 | 记录固件状态/计数 |

**关键约束**：`BL_BOOT_BASE` / `BL_APP_BASE` / `BL_META_BASE` 必须正好落在扇区边界（Flash 只能整扇区擦）。配置区放末尾（F4 末尾是整块大扇区，开头几个小扇区凑不出干净区域）。文件末尾有编译期 `#error` 自检。

APP 固件的起始地址 = `BL_APP_BASE` = **0x08004000**（网页上会提醒用户固件必须编译到这个地址）。

---

## 5. 启动决策流程（`core/bl_boot.cpp` 的 `decide()`）

判定顺序（顺序本身是安全设计的一部分）：

```
1. 固件状态可跳转？          → 否（Invalid / Download）进 IAP，无限等待
2. 软件复位 + Valid 态？     → 进「15s 限时窗口」（APP 唤回主通道）
3. 向量表合法？              → 否进 IAP
4. 整镜像 CRC32 匹配？       → 否进 IAP
5. 全部通过                  → 跳转 APP
```

第 3 步限定 `state==Valid` 是必须的：**升级完成后 Bootloader 自己也会软复位**
（旧版本如此；现在改为直接 `Boot::jump`，见 §6.3），若不加限定，固件
Download 态下的软复位也会被当成"APP 唤回"而误进窗口。

**复位原因每次启动必读**（read-and-clear 语义）：`RCC_CSR` 的复位标志是
累积的，只有读才清。漏读会让标志粘住，下一次启动被误判。

---

## 6. 状态机与升级的原子提交

### 6.1 固件状态机

```
Invalid ──(mark_download)──► Download ──(commit)──► Valid
```

- `Invalid`：配置区空 / 无有效记录 → 不可跳转，进 IAP
- `Download`：正在写、或写到一半掉电 → 不可跳转，进 IAP 可重刷
- `Valid`：写入完成且校验通过 → 可跳转

没有 Testing / Revoked 两个中间态 —— 那两个是旧版"自确认 + 回滚"机制的
产物，已删除。

### 6.2 升级的原子提交（`core/bl_session.cpp`）

顺序不可颠倒，否则中途掉电会留下"状态可跳转但 APP 已擦空"的组合：

```
① meta.mark_download()  → 置 Download（不可信）
② 擦除 APP 区所需扇区
③ 接收 YMODEM 并写入
④ 边收边算整镜像 CRC32
⑤ meta.commit()         → 置 Valid，记录 size / crc32 / 版本
```

①必须在②之前。

### 6.3 升级完成后直接跳转

`app/bl_entry.cpp` 在 `outcome==Done` 后**直接 `Boot::jump()`** 跳新固件，
不再 `system_reset()` 重新走一遍决策。这样只有一条跳转路径，也不再有
"升级完成的软复位"与"APP 唤回软复位"的语义冲突。

---

## 7. 软件复位唤回 + 15s 限时窗口（最近的机制，取代了旧的 RAM 标志）

**要解决的问题**：APP 运行中怎么优雅回到 Bootloader 刷新固件。

**流程**：
1. 网页发关键字 `#Bootloader-Everywhere`（逐字节慢发，见下）
2. APP 检测到完整关键字 → 只软复位（`NVIC_SystemReset`，不写任何标志）
3. Bootloader 读复位原因 = 软件复位 + 固件 Valid 态 → 进 15s 限时窗口
4. 窗口内等 YMODEM 首包，等到就升级；15s 超时跳回 APP
5. 正常上电/硬件复位零等待直接跳 APP

**关键点**：
- `core/bl_boot.cpp` 第 2 步：`cause==Software && state==Valid` 才进窗口
- `core/bl_ymodem.cpp`：握手总超时 `handshake_timeout_ms`（默认 15s，`BL_YMODEM_HANDSHAKE_MS`）。握手阶段没等到首包就 `Status::Timeout`；`=0` 表示关闭（无限等）
- `app/bl_entry.cpp`：限时窗口超时（`Failed + Timeout + fw_size==0`）→ `Boot::jump()` 跳回 APP

### ⚠️ 复位原因的一个大坑（探针拖 PIN 复位）

**实测**：DAPLink 连着时，APP 软复位（SYSRESETREQ）会连带把 NRST 拉一下，导致 `RCC_CSR` 里 `SFTRSTF` 和 `PINRSTF` **同时置位**（实测 `0x14000000`）。

`target/stm32f4/bl_port_system_stm32f4.cpp` 的 `reset_cause()` **必须把 `SFTRSTF` 排在 `POR/PIN` 之前**（软件复位是更明确的意图）。否则软复位被误判成引脚复位，唤回通道在开发期失灵。

副作用：探针的 `reset_and_halt` 也会置 `SFTRSTF`，所以**探针复位也会被当成软复位进窗口**。这只影响开发期调试，不影响真实产品（网页刷机探针不连）。

---

## 8. 已删除的机制（不要加回来）

以下东西在 2026-10-05 按用户要求**整体删除**，本仓库不应再出现：

| 已删除 | 原因 |
|---|---|
| 看门狗 IWDG（`BL_USE_WATCHDOG` / `wdg_init` / `wdg_feed`） | IWDG 一旦启动无法停止，会强迫所有 APP 必须喂狗；对 IAP 场景是纯负担 |
| 自确认（`BL_BOOT_SELF_CONFIRM`）与回滚（`BL_BOOT_MAX_ATTEMPTS` / `revoke`） | 依赖看门狗复位作判据；且 IAP 是人在场操作，不需要自动回滚 |
| `FwState::Testing` / `FwState::Revoked`、`boot_attempts`、`confirm_app()` | 上面两项的配套状态 |
| Backdoor 时间窗（`wait_backdoor` / `BL_BACKDOOR_WINDOW_MS`） | 已被「软件复位唤回 + 15s 窗口」取代 |
| 配置区请求升级（`request_update` / `kFlagUpdateReq`） | 无调用点，被关键字唤回取代 |
| `uart_set_baudrate` / `console_uart` / `system_reset` / `IapResult::Running` | 预留但从未使用的死代码 |

**如果 APP 不喂狗会不会被 Bootloader 反复复位？** 不会 —— 已经没有看门狗了。
坏固件（能过 CRC 但一跑就崩）刷进去后设备会卡住，需要人工重新上电进 IAP 再刷。
这是明确接受的取舍。

---

## 9. 构建系统

### 9.1 Keil MDK（编译 + 下载，最常用）

工程：`MDK-ARM/LUMOS-bootloader.uvprojx`。UV4 路径：`C:\Users\11846\AppData\Local\Keil_v5\UV4\UV4.exe`

```bat
UV4 -r proj.uvprojx -j0 -o build.log   :: 全量重建
UV4 -b proj.uvprojx -j0 -o build.log   :: 仅编译
UV4 -f proj.uvprojx -j0 -o flash.log   :: 编译 + 下载（烧到板子）
```

退出码：0=无错无警告，1=有警告，2=有错误。日志搜 `error:` 与末尾 `Program Size`。

### 9.2 命令行构建 `tools/build.py`（不打开 uVision）

```bash
python Bootloader/tools/build.py                 # 默认芯片，量 ROM
python Bootloader/tools/build.py -c stm32f405xx  # 换芯片
python Bootloader/tools/build.py --all           # 逐个芯片跑，对比 ROM 占用
```

依赖 Keil AC6 工具链（armclang/armasm/armlink/fromelf），路径可用 `KEIL_ARMCLANG_BIN` 覆盖。

### 9.3 测试固件 `TestApp/build_app.py`（test-app 分支）

```bash
python TestApp/build_app.py --fail 0   # 正常固件 app_test.bin
python TestApp/build_app.py --fail 1   # 挂死     app_fail1.bin
python TestApp/build_app.py --fail 2   # HardFault app_fail2.bin
```

---

## 10. 板端验证环境（STM32F401RET6）

- **编译**：Keil MDK（本地 Windows）
- **烧写**：DAPLink（CMSIS-DAP，`0d28:0204`）。⚠️ **pyocd 烧写经常 `result code 0x1`（线缆信号完整性老问题），优先用 `UV4 -f` 下载**（Keil 自带驱动 + Pack 算法，绕过了 pyocd 的坑）
- **串口**：CH340 USB 转串口，COM3 @115200 8N1
- **供电**：独立 Type-C 5V

板端工具（`Bootloader/tools/board.py`，用 pyocd + pyserial）：
```bash
python tools/board.py info        # 芯片信息 + 分区占用 + 配置区状态
python tools/board.py backup      # 整片备份
python tools/board.py monitor     # 读串口
python tools/board.py flash build/bl_f401.bin
```

烧写失败的排查见 skill `stm32-swd-flash-triage`（核心结论：先试 Keil，再看线缆长度）。

---

## 11. 移植新芯片（重点）

**先读 `Bootloader/docs/PORTING.md`，它是最权威的。以下是总览。**

设计目标：**同一份 `Bootloader/` 目录，原样拷到任何芯片的工程都能用**（不是"一个工程同时支持多芯片"）。芯片差异全部赶到少数几个文件。

### 三层（换芯片时哪些要动）

| 层 | 目录 | 换芯片时 |
|---|---|---|
| 应用层 | `app/` | 不动 |
| 核心层 | `core/` | **一行都不动** |
| 配置层 | `bl_config.h` + `presets/` | 选预置或填几个数 |
| 移植层 | `port/` + `target/` | **唯一写代码的地方** |

### 移植四步

1. **拷目录**：整个 `Bootloader/` 拷进目标工程，只需一个 include 路径 `-I <工程>/Bootloader`
2. **选芯片**：`bl_config.h` 第一节三选一 —— A. 复用 HAL 的 CMSIS 器件宏（推荐，什么都不用做）/ B. 取消 `#include "presets/xxx.h"` 注释 / C. 没有预置就照格式自己写或填「手填区」（`BL_FLASH_SIZE`/`BL_SRAM_END`/时钟参数）。填漏编译报错，不会带错值上板
3. **加编译 + 接入口**：加入编译 17 个文件（core 7 个 + app 1 个 + target 若干，见 PORTING.md 清单）；**不要**编译 `port/bl_port_template.cpp`；入口用库自带 `main()` 或设 `BL_PROVIDE_MAIN=0` 接自己 main
4. **告诉 APP 侧**：APP 工程 IROM1 起始 = `BL_APP_BASE`，`main()` 开头 `SCB->VTOR = BL_APP_BASE`；若要支持"运行中被唤回"，串口收齐关键字 `#Bootloader-Everywhere` 后调 `bl_request_update()`（见 `bl_app.h`，只依赖 CMSIS）

### 两类移植

- **同族芯片（复用 target）**：F401/F405/F407/F411/F415/F417 共用 `target/stm32f4/`，**只改 `bl_config.h` 几个数字**（扇区规则、编程单位都相同）
- **异族芯片（写 target）**：复制 `port/bl_port_template.cpp` 到 `target/新平台/`，按 19 个函数注释逐项填。差异点举例：擦除单位（F4 不等长扇区 vs GD32 等长页）、编程单位（F4 32 位字 vs GD32 16 位半字）、时钟树。`core/` 完全不动

**时钟参数必须与 `stm32f4xx_hal_conf.h` 的 `HSE_VALUE` 一致**，否则串口乱码（库有编译期 `#error` 挡住）。RISC-V 内核（CH32V307）注意向量表不是 `SCB->VTOR`，不能照搬 Cortex-M。

移植自查清单见 PORTING.md 第五节。

---

## 12. 测试（test-app 分支的 `TestApp/tools/`）

| 脚本 | 作用 |
|---|---|
| `test_auto.py` | 一键跑 T1-T5，逐项判定 PASS/FAIL（正常升级 / 连续升级 x5 / 软件复位唤回 / 窗口超时跳回 APP / 传输中断不变砖） |
| `verify_window.py` | 验证「软件复位唤回 + 15s 窗口」链路 |
| `power_test.py --phase N` | 断电暴力测试引导（5 个断电机时，用户手动拔电配合） |
| `board_test.py` | 烧写/进 IAP/升级/观察 的基础函数 |
| `ymodem_send.py` | YMODEM 发送端（协议核心，与网页 ymodem.js 对齐） |

真板实测（STM32F401RET6）当前 T1-T5 全通过。

完整失效模式清单见 `Bootloader/docs/TEST_PLAN.md`（含 18 次断电暴力测试计划）。

---

## 13. 网页（web 分支的 `docs/`）

- 纯前端（Web Serial + YMODEM），挂 GitHub Pages，无后端
- 部署：Settings → Pages → Source 选 `web` 分支 `/docs`
- 浏览器要求：Chrome/Edge 89+（Web Serial），必须 https 或 localhost
- **关键字唤回**：点「开始升级」→ `wakeUpBootloader()` 逐字节慢发 `#Bootloader-Everywhere`（每字节 150ms，因为 APP 主循环 100ms 轮询 + 单字节缓冲，连发会 overrun）→ APP 软复位 → 15s 窗口内发 YMODEM
- 语法检查：`node --check docs/js/app.js`

---

## 14. 常见坑（都实测过，接手时注意）

1. **探针拖 PIN 复位**（见 §7）：软复位时 `SFTRSTF`+`PINRSTF` 同时置位，`reset_cause` 必须让 `SFTRSTF` 优先
2. **轮询模型多字节命令**：APP 100ms 轮询 + 单字节缓冲，上位机连发会 overrun，必须逐字节慢发
3. **pyocd 烧写 result 0x1**：线缆信号完整性，优先用 `UV4 -f` 下载
4. **切分支后 Bootloader/ 文件丢失**：`git checkout HEAD -- Bootloader/` 恢复
5. **批量 Edit 部分丢失**：同一消息里对同一文件发多个 Edit，可能静默丢失部分，改完务必 grep/Read 验证落盘
6. **`HSE_VALUE` 不一致**：串口满屏乱码（HAL 反算波特率错）
7. **配置区残槽会让升级永远失败**（已修，但换芯片时注意）：`Meta::init()` 若无有效槽且区内有脏数据，必须先擦除整片再从头写，否则 `mark_download` 写偏移 0 会撞上未擦除的 Flash
8. **标准 printf 撑爆 ROM**：`vsnprintf` 连带浮点格式化吃 6.5KB，必须用 `core/bl_log`
9. **栈太小**：`Session` 带 1KB YMODEM 缓冲，栈不能小于 2KB
10. **测试脚本别盲等 ACK**：bootloader 会周期性补发 `C`，用 `wait_byte` 盲等容易读错字节；按日志文本（如 `[session] file=`）判定更稳

---

## 15. commit 约定

- 作者固定：`kkl <1184665829@qq.com>`
- 信息格式：
  ```
  @操作(关键词): 详细修改信息

  Co-Authored-By: Claude <noreply@anthropic.com>
  ```
  `@` 后接操作类型（`update`/`add`/`delete`/`fix`/`refactor`/`feat` 等），`()` 内填关键词，署名行前空一行。

---

## 16. 接手后的操作清单

1. `git status` + `git branch -a` 确认当前分支、工作区是否干净
2. 若 `Bootloader/` 文件丢失，先 `git checkout HEAD -- Bootloader/` 恢复
3. 明确任务属于哪个分支（库→main，测试→test-app，网页→web）
4. 改完编译验证：库用 `UV4 -r` 或 `tools/build.py`；测试固件用 `build_app.py`；网页用 `node --check`
5. 上板验证：`UV4 -f` 下载 + 串口 COM3 观察（库改动必上板，光编译不算完）
6. 提交按 §15 格式，推送对应分支
7. 每次迭代后更新 README 并 push（文档是交付的一部分）
