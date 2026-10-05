# AGENTS.md — LUMOS-bootloader 项目接手指南

> 写给接手的 AI（Agent）。目标：**只看这一份就能理解项目、继续开发、移植新芯片。**
> 面向使用者的文档是 `README.md`（仓库根）与 `Bootloader/README.md`（库文档）。

---

## 1. 这是什么

**LUMOS-bootloader** —— 一个可搬走的串口 IAP Bootloader。

- YMODEM-1K 串口刷固件：接收 → 校验（向量表 + 整镜像 CRC32）→ 跳转
- 固件状态机 `Invalid → Download → Valid`，靠**提交顺序**保证掉电不变砖
- APP 运行中可被唤回：上位机发关键字 `#Bootloader-Everywhere`，APP 软复位，
  Bootloader 靠**复位原因**识别并开 15s 限时窗口
- 网页上位机（Web Serial）：**https://zhangkeliang0627.github.io/Bootloader-Everywhere/**
- GitHub：`ZhangKeLiang0627/Bootloader-Everywhere`（public）

**设计取向（用户拍板，别改回去）**：定位是 IAP 而不是 OTA，**不做自动回滚、
不用看门狗**。刷进坏固件就再刷一次。详见 §6。

---

## 2. 仓库结构

```
README.md              入口导航（"从哪开始"）
AGENTS.md / USER.md    给 AI / 给使用者
Bootloader/            ★ 库本体，6 个文件，0 子目录
  README.md              库的唯一文档（含移植指南、常见坑）
  bl.h                   唯一入口（bl_run / bl_request_update / 公共类型）
  bl.cpp                 全部实现（日志/CRC/元数据/校验/YMODEM/会话/决策/入口）
  bl_port.h              移植契约（要实现的函数 + 填空说明）
  bl_port_stm32f4.cpp    STM32F4 实现（含板级配置：串口/引脚）
  bl_config.h            编译期配置（芯片预置 / 分区 / 超时）
tools/                 开发工具（不属于库）
  build.py               命令行编译 + 量 ROM（armclang）
  board.py               pyocd 板端操作（备份/烧写/擦除/看串口）
docs/HISTORY.md        立项期测试方案与开源调研（历史资料）
Core/ Drivers/ Bsp/ UserApp/ MDK-ARM/ LUMOS-bootloader.ioc
                       STM32F401 示例工程（CubeMX + Keil），非库的一部分
build/                 编译产物
```

**库与示例的分界**：想用库只需拷 `Bootloader/`。示例工程只是"怎么接进真实工程"
的演示 —— **是库去配合别人的工程，不是别人配合这个仓库**（用户明确要求）。

---

## 3. 三个分支

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

## 4. 库内部分工（改了代码放哪）

| 想改什么 | 改哪 |
|---|---|
| 分区 / 波特率 / 芯片型号 / 超时 | `bl_config.h` |
| 串口用哪一路、哪几个脚 | `bl_port_stm32f4.cpp` 顶部的「板级配置」 |
| 协议、升级流程、决策逻辑 | `bl.cpp` |
| 换芯片 | 照 `bl_port_stm32f4.cpp` 再写一份；`bl.cpp` 一个字不改 |

`bl.cpp` 的章节顺序就是"上电后会发生什么"：①日志 ②CRC ③元数据 ④校验
⑤YMODEM ⑥会话 ⑦决策 ⑧入口。找东西按这个顺序翻。

**铁律**：`bl.cpp` 里**不得出现任何芯片厂商头文件**。可自动验证：

```bash
grep -E '#include *[<"](stm32|gd32|ch32|hal)' Bootloader/bl.cpp   # 必须无输出
```

---

## 5. 核心机制

### 5.1 启动决策（`bl.cpp` ⑦ 决策节）

```
① 固件状态可跳转？       否（Invalid/Download）→ 进 IAP，无限等
② 软件复位 + Valid？     是 → 进「15s 限时窗口」（APP 唤回主通道）
③ 向量表合法？           否 → 进 IAP
④ 整镜像 CRC32 匹配？    否 → 进 IAP
⑤ 全通过                 → 跳 APP
```

第 ② 步限定 `state == Valid` 是必须的：否则"升级完成后的复位"也会被当成唤回。

`reset_cause()` **每次启动必读**（read-and-clear）：`RCC_CSR` 的复位标志是累积的，
只有读才清。漏读会让标志粘住，下次启动误判。

### 5.2 升级的原子提交（`bl.cpp` ⑥ 会话节）

```
① meta.mark_download()  置 Download（先让固件"不可信"）
② 擦除 APP 区
③ 接收 YMODEM 并写入
④ 边收边算 CRC32
⑤ meta.commit()         置 Valid + 记录 size/crc32/版本 → 直接跳转
```

①必须在②之前。顺序反了，擦除中途掉电会留下"状态可跳转但 APP 是空片"的必砖组合。

### 5.3 唤回窗口

- `bl.cpp` 握手超时 = `BL_YMODEM_HANDSHAKE_MS`（15s）；置 0 = 无限等
- 窗口内没等到首包 → 跳回 APP（判据：`Failed + Timeout + fw_size == 0`）
- 正常上电/按复位 → 零等待直接跳 APP

### 5.4 配置区（`bl.cpp` ③ 元数据节）

日志式追加槽位：每次状态变更顺序写一个新槽而不擦除，读的时候取序号最大的
有效槽，写满整片才擦一次。128KB / 64B = 2048 个槽。

---

## 6. ⚠️ 刻意删掉的机制（不要加回来）

2026-10-05 按用户要求整体删除，**本仓库不应再出现**：

| 已删除 | 原因 |
|---|---|
| 看门狗 IWDG（`BL_USE_WATCHDOG`/`wdg_init`/`wdg_feed`） | 一旦启动无法停止，会强迫所有 APP 必须喂狗；对 IAP 是纯负担 |
| 自确认 + 回滚（`BL_BOOT_SELF_CONFIRM`/`MAX_ATTEMPTS`/`revoke`） | 依赖看门狗复位作判据；IAP 是人在场操作，不需要自动回滚 |
| `FwState::Testing`/`Revoked`、`boot_attempts`、`confirm_app()` | 上面两项的配套状态 |
| Backdoor 时间窗（`wait_backdoor`） | 已被「软件复位唤回 + 15s 窗口」取代 |
| 配置区请求升级（`request_update`/`kFlagUpdateReq`） | 无调用点，被关键字唤回取代 |
| `uart_set_baudrate`/`console_uart`/`system_reset`/`IapResult::Running` | 预留但从未使用的死代码 |

删掉看门狗后，**APP 不需要也不能喂狗**；坏固件刷进去会卡住，人工重上电即可。

---

## 7. 构建与验证

### 7.1 Keil（编译 + 下载，最常用）

工程：`MDK-ARM/LUMOS-bootloader.uvprojx`；UV4：`C:\Users\11846\AppData\Local\Keil_v5\UV4\UV4.exe`

```bat
UV4 -r proj.uvprojx -j0 -o build.log   :: 全量重建
UV4 -f proj.uvprojx -j0 -o flash.log   :: 编译 + 下载（烧到板子）
```

退出码 0=无错无警告，1=有警告，2=有错误。日志搜 `error:`。

语言标准：**C++11**（Keil 里由 Options → C/C++ → Language C++ 控制，对应
`<v6LangP>3</v6LangP>`）。**不要往 MiscControls 里加 `-std=c++11`** ——
那份设置会同时作用到 C 文件上，armclang 会报 `not allowed with 'C'`。

### 7.2 命令行量 ROM

```bash
python tools/build.py            # 默认芯片，量 ROM
python tools/build.py --all      # 逐芯片对比
```

依赖 Keil AC6 工具链（armclang/armasm/armlink/fromelf），可用 `KEIL_ARMCLANG_BIN` 覆盖。

### 7.3 测试（test-app 分支）

```bash
python TestApp/build_app.py --fail 0   # 正常固件
python TestApp/build_app.py --fail 1   # 挂死固件
python TestApp/build_app.py --fail 2   # HardFault 固件
python TestApp/tools/test_auto.py      # 一键 T1-T5，逐项判 PASS/FAIL
python TestApp/tools/power_test.py --phase N   # 断电暴力测试（人工拔电配合）
```

当前 T1-T5 真板全通过（F401RET6）：正常升级 / 连续升级 x5 / 软件复位唤回 /
窗口超时跳回 APP / 传输中断不变砖。

### 7.4 板端环境

- 编译：Keil MDK（本地 Windows）
- 烧写：DAPLink（CMSIS-DAP，`0d28:0204`）。⚠️ **pyocd 烧写经常 `result code 0x1`
  （线缆信号完整性），优先用 `UV4 -f`**（Keil 自带驱动绕过该问题）
- 串口：CH340，COM3 @115200 8N1；供电：独立 Type-C 5V

---

## 8. 移植新芯片

**先读 `Bootloader/README.md` 的「移植到别的芯片」，那里是权威。**

要点：
- 同族（F401/F405/F407/F411/F415/F417）→ 只改 `bl_config.h` 第一节
- 异族 → 复制 `bl_port_stm32f4.cpp`，照 `bl_port.h` 的注释实现；
  差异集中在擦除单位（F4 不等长扇区 vs GD32 等长页）、编程单位（32 vs 16 位）、
  时钟树
- **RISC-V 内核（CH32V307）**：向量表不是 `SCB->VTOR`，跳转流程要查手册
- 时钟参数必须与工程里的 `HSE_VALUE` 一致，否则串口乱码（库有 `#error` 挡）

---

## 9. 常见坑（都实测过）

1. **探针拖 PIN 复位**：DAPLink 连着时软复位会连带置 `PINRSTF`，
   `reset_cause()` 必须让 `SFTRSTF` 优先，否则唤回在开发期失灵
2. **配置区残槽**：无有效槽且区内有脏数据时，必须先擦整片再从头写，
   否则 `mark_download` 写偏移 0 撞上未擦除 Flash，表现为"永远升级失败"
3. **pyocd 烧写 0x1**：优先 `UV4 -f`
4. **切分支后 Bootloader/ 文件丢失**：`git checkout HEAD -- Bootloader/`
5. **批量 Edit 部分丢失**：同一消息里对同一文件发多个 Edit 可能静默丢，改完务必验证落盘
6. **`HSE_VALUE` 不一致**：串口满屏乱码
7. **标准 printf 撑爆 ROM**：`vsnprintf` 连带浮点吃掉 6.5KB，必须用 `BL_LOG`
8. **栈太小**：`Session` 带 1KB YMODEM 缓冲，栈不能小于 2KB
9. **测试脚本别盲等 ACK**：bootloader 会周期性补发 `C`，按日志文本判定更稳
10. **别用 `-std=` 覆盖 C 语言**：见 §7.1

---

## 10. commit 约定

- 作者固定：`kkl <1184665829@qq.com>`
- 格式：
  ```
  @操作(关键词): 详细修改信息

  Co-Authored-By: Claude <noreply@anthropic.com>
  ```
  `@` 后接 `update`/`add`/`delete`/`fix`/`refactor`/`feat` 等，署名行前空一行。

---

## 11. 接手后的操作清单

1. `git status` + `git branch -a`；若 `Bootloader/` 有文件丢失先恢复
2. 明确任务属于哪个分支（库→main，测试→test-app，网页→web）
3. 改完编译：库用 `UV4 -r` 或 `tools/build.py`；网页用 `node --check docs/js/app.js`
4. **库改动必须上板**（`UV4 -f` + COM3 观察 + `test_auto.py`），光编译不算完
5. 提交按 §10，推送对应分支；文档变更三分支同步
6. 每次迭代后更新 README（文档是交付的一部分）
7. **大改动先提交再删旧文件** —— 本项目踩过一次"新旧交替时误删未提交的新文件"
