# 网页上位机

纯前端、零后端的串口升级工具。用浏览器的 **Web Serial API** 直接操作串口，
把编译好的 `.bin` 按自定义 **0xA5 帧协议**发给开发板 —— 也就是 `protocol-v2` /
`web-v2` 分支上那套固件用的协议。部署到 GitHub Pages 后，有网、有 Chrome/Edge
的地方就能刷固件。

## 文件

```
docs/
├── index.html        页面骨架
├── css/style.css     样式（浅色/深色自适应）
├── js/
│   ├── protocol.js   载体层 + IAP 客户端（与固件 protocol.cpp、proto.py 同口径）
│   └── app.js        Web Serial 适配 + 界面逻辑
└── PROTOCOL_DESIGN.md  协议规格（§0 是现行设计）
```

`protocol.js` **不依赖浏览器**（只用标准 API），可以在 Node 里直接跑单测。
三处实现（固件 C++ / 命令行 Python / 网页 JS）共用同一组测试向量：

```bash
node tools/test_protocol_js.mjs          # 载体层：CRC8 / CRC32 / 组帧 / 解析 / 重同步 / 帧长自检
node tools/test_iap_sim.mjs              # IAP 逻辑：用虚拟从机跑完 START → DATA → END
python TestApp/tools/proto.py selftest   # Python 侧同一组向量
```

> 目录名是 `docs/` 而非 `web/`：GitHub Pages 的 legacy source 只认根目录或
> `/docs` 文件夹，用这个约定目录才能免 workflow 直接发布。

## 本地跑

静态服务器起一下即可（Web Serial 要求安全上下文，`localhost` 算安全）：

```bash
cd docs
python -m http.server 8080
# 浏览器打开 http://localhost:8080
```

> 不要直接双击 `index.html` —— `file://` 不是安全上下文，Web Serial 会被禁用。

## 部署到 GitHub Pages

**已上线：<https://zhangkeliang0627.github.io/Bootloader-Everywhere/>**

发布源 = **`web-v2` 分支的 `/docs`**（仓库 Settings → Pages → Source 选
`Deploy from a branch`，Branch `web-v2` / Folder `/docs`）。GitHub Pages 的
legacy source 只能指仓库根目录或 `/docs`，所以页面必须待在这个约定目录里。

改完页面后 `git push origin web-v2`，Pages 会自动重建，一两分钟后刷新即可。

> ⚠️ 发布分支决定了线上页面用哪套协议。现在的线上页是 **0xA5 帧**版，只能刷
> `web-v2` / `protocol-v2` 上的固件；`web` 分支存的是旧的 **YMODEM** 版页面
> （**已停用，不再是发布源**）。两套互不兼容，切换时别搞混。
>
> `docs/` 里同时放着协议规格（`PROTOCOL_DESIGN.md` / `PERF_COMPARISON.md`）与网页文件，
> **两者都会被发布** —— 这是有意的（规格有稳定 URL 可引用，本节上面就链到它）。
> 若不想发布它们，在 `docs/` 下加一个 `_config.yml`，用 Jekyll 的 `exclude` 排除即可。

## 使用要点

- **必须用 Chrome / Edge**（Firefox / Safari 不支持 Web Serial）。
- **波特率固定 115200**，由固件写死、不做协商 —— 页面里也去掉了选择项，
  因为选了别的值只会连不上。
- **从机地址**对应固件里的 `BL_DEVICE_ID`（默认 1）。页面上可以改，用于多从机场景。
- 板子不必预先处于 IAP：点「开始升级」会先发关键字 `#Bootloader-Everywhere`
  把 APP 唤回 Bootloader（软复位进 15s 限时窗口），然后自动开始传输。
  板子本来就在 IAP 时，这些关键字字节会被当成帧间噪声丢掉，无害。
- 传输期间板端会**关掉日志**（日志与协议共用同一个串口，开着会污染本页的接收流），
  所以「串口输出」面板在传输中是安静的，这是正常的。
- 空闲时页面上显示的才是板端的文本日志。

## 协议要点

| 项 | 值 |
|---|---|
| 帧格式 | `[0xA5] [ID] [CMD] [len:2 LE] [Data ≤1024] [CRC8] [0x03]`，共 7 + N 字节 |
| 帧长范围 | 7 - 1031 字节；超出即丢帧 |
| 校验 | CRC-8/SMBUS（poly 0x07，MSB-first）；覆盖 `[ID .. Data]`，不含头尾 |
| 命令 | `0x01` START / `0x02` DATA / `0x03` END / `0x04` STATUS，应答置 bit7 |
| 块大小 | 512 字节（DATA 一应一答） |
| 整片校验 | CRC32/ISO-HDLC，覆盖镜像 `[8, size)`；由**板端回读 Flash** 重算 |
| DATA 应答 | 统一 `[code][cumCrc32:4][nextAddr:4]`（含 0x07 跳号） |
| 断点续传 | 出错时板端在应答里给出期望地址，主机从那里续发 |

详细规格见 [`PROTOCOL_DESIGN.md`](PROTOCOL_DESIGN.md) §0。命令行版参照实现
见 [`../TestApp/tools/proto.py`](../TestApp/tools/proto.py)。
