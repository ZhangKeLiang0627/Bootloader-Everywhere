# 网页上位机

纯前端、零后端的 YMODEM 串口升级工具。用浏览器的 **Web Serial API** 直接操作
串口，把编译好的 `.bin` 发给开发板。部署到 GitHub Pages 后，有网、有 Chrome/Edge
的地方就能刷固件。

## 文件

```
docs/
├── index.html      页面骨架
├── css/style.css   样式（浅色/深色自适应）
└── js/
    ├── ymodem.js   YMODEM-1K 发送端（协议核心，与 TestApp/tools/ymodem_send.py 对齐）
    └── app.js      Web Serial 适配 + 界面逻辑
```

> 目录名是 `docs/` 而非 `web/`：GitHub Pages 的 legacy source 只认根目录或
> `/docs` 文件夹，用这个约定目录才能免 workflow 直接发布。

## 本地跑

直接用静态服务器起一下即可（Web Serial 要求安全上下文，`localhost` 算安全）：

```bash
cd docs
python -m http.server 8080
# 然后浏览器打开 http://localhost:8080
```

> 不要直接双击 `index.html` —— `file://` 不是安全上下文，Web Serial 会被禁用。

## 部署到 GitHub Pages

1. 仓库转 **public**（免费账户的私有仓库开不了 Pages）。
2. Settings → Pages → Source 选 `Deploy from a branch`：
   - Branch：`web`
   - Folder：`/docs`
3. 保存后等一两分钟，页面地址形如
   `https://<user>.github.io/Bootloader-Everywhere/`。

## 使用要点

- **必须用 Chrome / Edge**（Firefox / Safari 不支持 Web Serial）。
- 板子要处于 **IAP 等待状态**（串口周期性吐 `C`）。如果 APP 在正常跑，
  点「开始升级」会自动发关键字 `#Bootloader-Everywhere` 把 APP 唤回
  Bootloader（软复位进限时窗口），无需手动复位或按任何键。
- 升级时**不需要**预先算 CRC32 —— 整镜像 CRC 由板端自算；
  帧级 CRC16 由协议自带，这里照规范实现即可。
- 板端调试日志与协议字节共用串口，页面会把它标成「板端」显示，别当成错误。

## 协议对照

| 项 | 值 |
|---|---|
| 协议 | YMODEM-1K（SOH 128B / STX 1024B）|
| 握手 | 收 `C`(0x43) → 发首包 |
| 首包 | 文件名 `\0` 十进制大小 |
| 校验 | CRC-16/XMODEM（poly 0x1021）|
| 首包 ACK 超时 | 12 s（板子要先擦完扇区）|
| 数据包 ACK 超时 | 3 s，最多重传 10 次 |
| 末包填充 | 0x1A（Ctrl-Z）|

命令行版参照实现见 `../TestApp/tools/ymodem_send.py`。
