# Bootloader-Everywhere

串口 IAP Bootloader，做成**可搬走的库**，外加一个能一键刷机的网页上位机。
任何时刻断电都不会变砖。

**在线刷机页：<https://zhangkeliang0627.github.io/Bootloader-Everywhere/>**
（Chrome / Edge，by Web Serial）

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
| **想用网页刷机** | 上面的在线地址；源码在 `web-v2` 分支的 `docs/` |
| 想跑板端测试 | [`TestApp/tools/`](TestApp/tools/) —— 见下面「测试」 |
| **接手开发（人或 AI）** | [`AGENTS.md`](AGENTS.md) |
| 只想快速上手用一下 | [`USER.md`](USER.md) |
