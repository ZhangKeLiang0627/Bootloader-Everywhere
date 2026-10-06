# Bootloader-Everywhere

串口 IAP Bootloader，做成**可搬走的库**，外加一个能一键刷机的网页上位机。
任何时刻断电都不会变砖。

**在线刷机页：<https://zhangkeliang0627.github.io/Bootloader-Everywhere/>**
（Chrome / Edge，用 Web Serial，不装任何软件 —— 已适配本分支的自定义 0xA5 帧协议）

---

## 一句话说明它怎么工作

上电只看 APP 区最前面的两个字（栈顶 SP + 复位入口 PC），而这两个字是
**升级过程中最后才写进去的** —— 所以"这两个字合法"就等于"上一份固件完整刷完了"。
传输中断、掉电、断流，这两个字都不会落盘，板子就停在 IAP 等你重刷。

不需要配置区、不需要状态标志、不需要看门狗。细节见
[`Bootloader/README.md`](Bootloader/README.md)。

---

## 从哪里开始

| 你想做什么 | 看这里 |
|---|---|
| **想用这个库 / 理解它** | [`Bootloader/README.md`](Bootloader/README.md) ← 从这里开始 |
| 想把它移植到别的芯片 | 同上，「移植（换芯片）」一节 |
| 想改 Flash 分区 | [`Bootloader/bl_config.h`](Bootloader/bl_config.h) |
| 想改帧协议 / 载体层 | [`Bootloader/protocol.h`](Bootloader/protocol.h)、[`protocol.cpp`](Bootloader/protocol.cpp) |
| 想改升级流程 / 启动决策 | [`Bootloader/bl.cpp`](Bootloader/bl.cpp) |
| 想改日志 / 开关日志 | [`Bootloader/bl_log.h`](Bootloader/bl_log.h) |
| **协议规格（帧格式 / 命令 / 错误码）** | [`docs/PROTOCOL_DESIGN.md`](docs/PROTOCOL_DESIGN.md) —— §0 是现行设计 |
| **性能实测与横向对比** | [`docs/PERF_COMPARISON.md`](docs/PERF_COMPARISON.md) |
| **想用网页刷机** | 上面的在线地址；源码与说明在 `web-v2` 分支的 `docs/`（本分支不含网页） |
| 想跑板端测试 | [`TestApp/tools/`](TestApp/tools/) —— 见下面「测试」 |
| **接手开发（人或 AI）** | [`AGENTS.md`](AGENTS.md) |
| 只想快速上手用一下 | [`USER.md`](USER.md) |

---

## 测试

从「不需要硬件」到「真板压测」共四层。前三层在 PC 上跑，用**同一组测试向量**
把三份实现（固件 C++ / 命令行 Python / 网页 JS）的口径钉死：

| 脚本 | 覆盖 | 结果 |
|---|---|---|
| `tools/test_protocol.cpp` | 载体层：CRC 向量 / 组帧 / 解析 / 重同步 / 帧长自检 | 45 项断言 0 失败 |
| `tools/stress_protocol.cpp` | 载体层压测：fuzz 32MB / 单字节突变 54 万次 / 对抗流 / 恢复能力 | 33 项断言 0 失败 |
| `TestApp/tools/proto.py selftest` | 与上同源向量（Python 侧） | 全过 |
| `tools/test_protocol_js.mjs`〔web-v2〕 | 与上同源向量（JS 侧） | 22 项断言 0 失败 |
| `tools/test_iap_sim.mjs`〔web-v2〕 | 用**虚拟从机**把 IAP 全流程跑一遍（不需要硬件） | 12 项断言 0 失败 |
| `TestApp/tools/test_proto.py` | 真板正常路径 T1-T5 | 5/5 |
| `TestApp/tools/test_proto_edge.py` | 真板边界与畸形输入 E1-E15 | 16/16 |
| `TestApp/tools/stress_iap.py` | 真板压测 S1-S13 | 13/13 |
| `TestApp/tools/test_proto_perf.py` | 2K - 496K 固件耗时实测 | 见 `docs/PERF_COMPARISON.md` |

标〔web-v2〕的两个脚本只在 `web-v2` 分支（它们 import `docs/js/protocol.js`，
而网页只存在于那个分支）。

真板测试项（STM32F401RET6）—— 括号里是它验证的那一层防线：

- **T1-T5**：正常升级 / 连续升级 ×5 / 传输中断（未提交 ⇒ 不变砖）/ 篡改帧被拒且可续传 / 跳号被拒并给出续传点
- **E1-E15**：超大固件 / 极小固件 / 向量表非法 / 各类字段不一致 / 载体层丢帧 / 坏固件被提交后仍能恢复。
  **每条都断言「被拒绝时 APP 区未被改动」** —— 这是「不会变砖」的直接证据
- **S1-S13**：连续升级 ×12 / 逐帧错误注入 / 应答丢失幂等 / 重放污染 / 跳号续传 / 突发帧 /
  空闲超时 / **END 整片回读校验**

---

## 仓库里有什么

```
Bootloader/        ★ 就是这个库，9 个源文件 / 1732 行，没有子目录
docs/              协议规格（PROTOCOL_DESIGN.md）+ 性能对比（PERF_COMPARISON.md）
                   （网页上位机在 web-v2 分支的 docs/，那里同时是 Pages 发布目录）
tools/             开发工具：编译量体积（build.py）、板端操作（board.py）、PC 侧压测
TestApp/           测试 APP 与板端测试脚本（真板验证都靠它）
AGENTS.md          给接手的人 / AI 的完整说明
USER.md            给使用者的快速上手

—— 以下是 STM32F401 示例工程，不是库的一部分 ——
Core/ Drivers/ MDK-ARM/ Bootloader-Everywhere.ioc
                   CubeMX + Keil 工程（含宿主 main），演示"库怎么接进真实工程"
build/             编译产物
```

**只想用这个库的话，只拷 `Bootloader/` 就够了** ——
是库去配合你的工程，不是你的工程来配合这个仓库。

---

## 分支

| 分支 | 内容 | 协议 | GitHub Pages |
|---|---|---|---|
| **`web-v2`** | 库 + 测试 + **网页上位机**（`docs/`） | **0xA5 帧** | ★ **发布源** |
| `protocol-v2` | 库 + 测试（不含网页） | 0xA5 帧 | — |
| `main` | 库 + STM32F401 示例工程 | YMODEM | — |
| `test-app` | 库 + 测试 APP 与脚本 | YMODEM | — |
| `web` | 旧网页（YMODEM 版） | YMODEM | 已停用 |

**线上页现在发布自 `web-v2` 的 `docs/`**（0xA5 帧版），所以网页刷机只对
`web-v2` / `protocol-v2` 上的固件有效。`main` / `test-app` 仍是 YMODEM 版，
**两套协议互不兼容**（帧格式与上位机都不同）。

`protocol-v2` 尚未合回 `main` / `test-app`。合并时记得：`web` 分支不再发布，
Pages 发布源要重新指一次。

| | YMODEM（`main` 等） | 0xA5 帧（`protocol-v2` / `web-v2`） |
|---|---|---|
| 协议层代码 | 508 行 / 1038 B | **`protocol.*` 238 行，零依赖可复用** |
| 整份固件 ROM | 13120 B | **10880 B** |
| 库本体（`tools/build.py` 量） | 8876 B | **7648 B** |
| 上位机 | 网页（Web Serial） | 网页 + `proto.py`（同一组向量） |
| 校验层数 | 帧 CRC16 + 整片 CRC32 | 帧 CRC8 + **逐帧回读** + 整片回读 |
| 恢复粒度 | 重传单包（1024 B） | 断点续传（512 B，按板端报的期望地址） |
| 帧格式可读性 | 低（双包长 / `~seq` 反码 / 空首包） | 高（`A5 / ID / CMD / len / Data / CRC8 / 03`） |
| 代价 | — | 失去"任何第三方串口工具都能刷" |
