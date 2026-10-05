# USER.md — 快速上手

> 给**使用者**的文档。想了解原理细节或接手开发，看 `AGENTS.md`。

## 快速上手（3 步刷机）

1. 用 **Chrome / Edge** 打开网页：
   **https://zhangkeliang0627.github.io/Bootloader-Everywhere/**
2. 点「连接串口」，选你的板子（115200 8N1）
3. 拖入 `.bin` 固件，点「开始升级」

网页会自动把板子唤回 Bootloader 再开始传输 —— **全程不用按键、不用手动复位**。

> ⚠️ 固件（.bin）必须编译到 **0x08004000** 起始（Bootloader 占了前 16KB），
> 大小不超过 496KB（F401）。起始地址不对，刷进去也跑不起来。

---

## 跳转原理（一句话版）

上电后 Bootloader 先看 APP 区最前面的**两个字**（栈顶 SP + 复位入口 PC）：

```
上电 → 读 APP 区前两个字 → 合法？ → 是 → 跳转 APP（0x08004000），零等待
                            └→ 否 → 留在 IAP 等 YMODEM 刷机
```

而这两个字是**升级过程中最后才写进去的**。所以"合法"就等于"上一份固件完整刷完了"：

| 板子处于什么状态 | 前两个字 | 上电行为 |
|---|---|---|
| 从没刷过 / 刚擦除 | `0xFFFFFFFF` | 留在 IAP |
| 传输写到一半 / 断流 / 掉电 | 仍是 `0xFFFFFFFF` | 留在 IAP，可重刷，**不会变砖** |
| 完整刷完 | 合法地址 | 跳 APP |

**这意味着不需要任何"配置区"**：状态不用存，写入顺序本身就是状态。

---

## 运行中的 APP 怎么回去刷机（唤回窗口）

老办法是"上电 300ms 内按某个键"，既慢又难卡准，已经废掉。现在是：

1. 网页点「开始升级」→ 向串口逐字节发关键字 `#Bootloader-Everywhere`
2. APP 匹配到完整关键字 → **自己做一次软件复位**
3. Bootloader 读复位原因，发现是软件复位 → 开一个 **15 秒的限时窗口**等 YMODEM
4. 窗口内收到首包就正常刷机；15 秒没等到东西，自动跳回 APP 继续跑
5. 正常上电 / 按复位键 → **零等待**直接跳 APP，没有任何启动延迟

---

## 你的 APP 侧需要做什么

**只有一件可选的事**：想支持"网页一键刷机"，就在串口收齐关键字后软复位。

库**不提供**这个接口 —— 关键字、匹配方式、要不要复位都由你自己定，
下面这段可以直接抄（零依赖，不需要包含任何库头文件）：

```c
#define BL_BOOT_KEY  "#Bootloader-Everywhere"

/* 在串口接收处理里逐字节喂进来，匹配完整就软复位 */
static void boot_key_feed(uint8_t ch)
{
    static uint32_t matched = 0;
    const char* k = BL_BOOT_KEY;

    if (ch == (uint8_t)k[matched]) {
        if (k[++matched] == '\0') {
            /* 匹配完 → 请求回到 Bootloader */
            *(volatile uint32_t *)0xE000ED0CUL = 0x05FA0004UL;  /* SCB->AIRCR = VECTKEY|SYSRESETREQ */
            for (;;) { }                                        /* 兜底 */
        }
    } else {
        matched = (ch == (uint8_t)k[0]) ? 1U : 0U;
    }
}
```

要点：

- 写 `SCB->AIRCR` 必须带 `VECTKEY = 0x05FA << 16`，否则硬件丢弃这次写入
- **IAP 期间串口要轮询收**：如果宿主开了 USART1 的中断收，先关掉收类中断
  （库的 `uartFlushRx()` 会顺带关，但自己的 APP 里要自己处理）
- 主循环里轮询的话，间隔要小于上位机两字节的发送间隔（网页端是逐字节慢发的）
- 样板见 `TestApp/app_main.c`（test-app 分支，纯寄存器实现）

库也**不需要**你做"确认固件"、"喂狗"之类的动作。

---

## 已知取舍（说清楚，免得踩坑）

- **刷进坏固件不会自动回滚**。能被正确刷进去、但一跑就崩的固件会被正常跳转进去
  然后卡住；这时**断电重上电**，前两个字仍然合法 → 还是跳进去（跑不起来就是跑不起来）。
  要重刷只能靠 APP 的唤回、或者 SWD。这是刻意去掉的复杂度，不是缺陷。
- **传输中断电是安全的**：前两个字没提交 → 留在 IAP，重新刷一次即可。
- **没有看门狗**，所以 APP 不需要（也无需）喂狗。
- **时钟与串口由你的工程配**（库一行初始化都不做）。串口记住 8N1、
  波特率与上位机一致（默认 115200），否则连不上。
- **不做启动时整镜像 CRC**：完整性由"提交前回读校验"保证（写入失败当场发现），
  但刷完之后发生的 Flash 位翻转不会在上电时被发现。

---

## 工具位置

| 工具 | 位置 | 用途 |
|---|---|---|
| **库本体（只需这 5 个文件）** | `Bootloader/` | bl.h / bl.cpp / bl_port.h / bl_port_stm32f4.cpp / bl_config.h |
| 库文档（提交机制、用法、移植、坑） | `Bootloader/README.md` | 权威文档 |
| 网页上位机 | `docs/`（web 分支） | 浏览器刷机，已上线 |
| 命令行构建 | `tools/build.py` | 编译 + 量 ROM |
| 板端工具 | `tools/board.py` | 备份/烧写/擦除/看串口 |
| 测试固件编译 | `TestApp/build_app.py` | 编正常/故障测试固件（test-app 分支） |
| 自动化测试 | `TestApp/tools/test_auto.py` | 一键跑升级/唤回/断流测试 |
| 断电测试引导 | `TestApp/tools/power_test.py` | 提示你拔电的断电暴力测试 |

> **移植到别的工程时，只拷 `Bootloader/` 这一个目录就够** —— 仓库里的
> `Core/ Drivers/ MDK-ARM/` 只是 STM32F401 的示例工程。

---

## 三个分支

| 分支 | 是什么 |
|---|---|
| `main` | Bootloader 库 + STM32F401 示例工程 |
| `test-app` | 库 + 测试 APP + 测试脚本 |
| `web` | 网页上位机（改完 push 自动更新线上页面） |

---

## 常用命令

| 做什么 | 命令 |
|---|---|
| 编译（改过源码时**必须**用这个） | `UV4 -r MDK-ARM/LUMOS-bootloader.uvprojx -j0 -o build.log` |
| 下载到板子 | `UV4 -f MDK-ARM/LUMOS-bootloader.uvprojx -j0 -o flash.log` |
| 量 ROM（不开 Keil） | `python tools/build.py` |
| 看板子串口 | `python tools/board.py monitor` |
| 编测试固件（test-app 分支） | `python TestApp/build_app.py --fail 0` |
