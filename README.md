# LocalSend Aero

**看腻了 Material You 设计的 LocalSend？**

**还在为 LocalSend 无法在老系统上运行而发愁？**

**这是一个用 Qt 重写的 Windows LocalSend 客户端 —— 原生控件、DWM 玻璃、完美运行在 XP ~ 11。**

> 本项目致敬并兼容 [LocalSend](https://github.com/localsend/localsend)。
> LocalSend 是一个优秀、开源、跨平台的局域网文件互传工具；LocalSend Aero 是它在
> **老版本 Windows** 上的一条替代路径：不用 Electron/Flutter，不要求 Win10+，
> 一个 32 位绿色单目录即可运行。

> ⚠️ 本项目与 LocalSend 官方**没有隶属关系**，不是官方发行版，也不代表官方立场。

<img width="688" height="530" alt="LocalSend Aero 在 Windows 7 上的运行截图" src="https://github.com/user-attachments/assets/71a82bf3-fd02-4c8a-b03f-a7c9f03cc7a2" />

---

## 两个招牌特点

### 使用 DWMAPI 实现透明，完美适配 Windows Vista / 7

窗口**不使用自绘边框**，而是保留系统原生的窗口框架（阴影、贴边、Aero Snap、系统标题栏按钮全部由 Windows 负责），
再用 DWM 把玻璃延伸到客户区：

```cpp
// 原生边框 + 逐像素透明 + 玻璃延伸，三者缺一不可
setAttribute(Qt::WA_TranslucentBackground);
DwmExtendFrameIntoClientArea(hwnd, &MARGINS{ -1, -1, -1, -1 });
```

于是：

* **Windows Vista / 7**：标题栏与四周露出真正的 Aero 玻璃，客户区上再画一块 `#CCFFFFFF` 的半透明圆角卡片，1 px 描边 —— 即「白色卡片浮在磨砂玻璃上」的效果；
* **Windows 8.x / 10 / 11**：系统玻璃被移除时自动退化为不透明底色，布局与控件不变；
* **使用了 [DWMBlurGlass](https://github.com/Maplespe/DWMBlurGlass) 以及 [OpenGlass](https://github.com/ALTaleX531/OpenGlass) 的 Windows 10 /11**：模糊样式完全跟随全局（例如 Acrylic 或 Mica） ；
* **Windows XP 以及无 / 禁用 DWM**：直接使用经典窗口与控件，功能完全一致；
* 运行中开关「桌面合成」（切基本主题、远程桌面）会即时切换，不会黑屏或错位。

玻璃与卡片是**同一个窗口**画出来的，没有第二层无边框窗口，所以拖动、最大化和系统动画都保持原生手感。

| Aero 开启 | 合成关闭 / XP |
| --------- | ------------- |
| <img width="688" height="530" alt="启用 Aero 的 LocalSend Aero 运行截图" src="https://github.com/user-attachments/assets/6c0e1339-f54b-4ed5-8204-b5cdc04ecc98" /> | <img width="656" height="498" alt="禁用 Aero 的 LocalSend Aero 运行截图" src="https://github.com/user-attachments/assets/07e80670-c9d2-48d5-a303-06d329402299" /> |

### 与官方 LocalSend 1.17.0 – 1.18.2 双向互通

完整实现了 LocalSend 的 HTTP 协议（`/api/localsend/v2/*`），不是「长得像」的替代品：

* 组播发现（`224.0.0.167:53317`）+ 子网扫描兜底（组播被交换机/防火墙吃掉时仍能找到对方）；
* HTTPS + 自签证书 + 双向 TLS，身份以证书 SHA-256 指纹为准；
* `chunked` 与 `Expect: 100-continue` 上传、`keep-alive` 连接复用、大文件流式落盘与 SHA-256 校验；
* 接收确认、PIN 码（含 3 次失败锁定）、取消通知（取消会通知对端停止）、文本消息（`preview` 字段）。

**互通矩阵（实测）**

| 对端                      | 发送 | 接收 | 文本消息 |
| ------------------------- | ---- | ---- | -------- |
| LocalSend **1.17.0**      | ✅    | ✅    | ✅        |
| LocalSend **1.18.2**      | ✅    | ✅    | ✅        |
| **另一个 LocalSend Aero** | ✅    | ✅    | ✅        |

测试环境：同一台机器的环回、局域网内多网卡（含 VMware 桥接网卡）与虚拟机对实体机；文件夹（含中文、空格、多级子目录）双向保持目录结构。

---

## 系统支持

| 系统                      | 状态                         |
| ------------------------- | ---------------------------- |
| Windows XP SP3（32 位）   | ✅ 可运行（无玻璃，经典控件） |
| Windows Vista / 7         | ✅ 玻璃 + 原生控件            |
| Windows 8 / 8.1 / 10 / 11 | ✅ 跟随系统合成状态           |
| 64 位系统                 | ✅ 32 位进程直接运行          |

**定位**：在老机器上，把它当作「低版本 Windows 也能用的 LocalSend 客户端」。
XP 上仍然会做 HTTPS（Qt 自带 OpenSSL 1.0.1j），因此它比老式 SMB 共享更安全一些，也不需要额外配置。

---

## 功能

**传输**

* 选文件、选文件夹（递归展开，接收端重建目录结构）、拖放（文件与文件夹）
* 多文件总进度（当前文件 + 总计，各带速度与剩余时间）
* 可随时取消；对端取消会立即停止并清理半成品
* 文本消息（单条，作为一条 `text` 类型消息）
* 接收确认对话框：可改保存位置、可记住「每次询问」
* 同名文件自动改名 `name (2).ext`，不覆盖已有文件
* PIN 码（4 位数字，连续 3 次错误后锁定）

**界面**

* 发送 / 历史 / 设置三页，Windows 通用控件库 6.0
* 中英双语（随系统或手动指定，重启生效）
* Vista+ 的提示与确认使用 `TaskDialog`
* 传输中窗口图标与托盘图标切换为「传输中」图标，结束后复原
* 多档 DPI 尺寸

**系统集成**

* 最小化/关闭到托盘，双击托盘显示主窗口，右键可显示或退出
* 登录时自动启动（`HKCU\...\Run`，带目录哈希避免多副本互相覆盖）
* 单实例（同目录同端口只允许一个；不同目录可并存，便于自测）
* 「添加设备」：直接输入 `IP` 或 `IP:端口` 手动连接，不依赖发现
* 刷新列表会真正校验在线状态（并行探测，离线设备立即消失）

**历史**

* 最近 200 条收发记录，机器可读地记录「根路径」（文件夹仍是文件夹）
* 右键：已接收 → 打开所在文件夹（在资源管理器中选中）；已发送 → 重新发送（恢复成原来的文件夹/文件）
* 双击：接收记录打开所在位置，发送记录重新加入发送列表

---

## 下载与运行

从 [Releases](https://github.com/ctrlmmc/LocalSend-Aero/releases) 下载 `LocalSendAero-<版本>-portable.zip`，解压到任意目录，双击 `lsaero.exe`。

* 不需要安装、不需要管理员权限；
* 配置写在 exe 同目录 `lsaero.ini`；目录不可写时回退到 `%APPDATA%\LocalSend Aero\lsaero.ini`；
* 首次运行在配置目录生成 `cert.pem` / `key.pem`（自签名，指纹即本机身份）；
* 日志 `lsaero.log`（上一轮保留为 `lsaero.log.1`）、历史 `history.jsonl` 都在同一目录；
* 防火墙：如果找不到别人，请在 Windows 防火墙里允许 `lsaero.exe` 的入站（专用/公用），
  程序不会自动改防火墙规则；
* 端口默认 `53317`，被占用时自动向后找（最多 20 个）。

---

## 从源码构建

**工具链**（VS2015 之前的运行库、无 UCRT 依赖）

* Qt **5.6.3**，`mingw49_32` 套件（i686，MinGW GCC 4.9.2）
* OpenSSL **1.0.1j**（Qt 官方安装包自带在 `Tools/mingw492_32/opt`）
* Windows PowerShell 5.1 或更新

```powershell
# 在仓库根目录
powershell -ExecutionPolicy Bypass -File tools\build.ps1 `
    -Pro  "C:\path\to\repo\src\lsaero.pro" `
    -Out  "C:\path\to\repo\build\lsaero"

# 产物：build\lsaero\release\lsaero.exe
```

打包绿色版：

```powershell
# 1) 拷 exe 到 dist\LocalSend Aero\
# 2) 用 windeployqt 收集 Qt 依赖（只需 Core/Gui/Widgets/Network + platforms + imageformats + translations）
windeployqt --release --no-angle --no-opengl-sw --no-svg "dist\LocalSend Aero\lsaero.exe"
# 3) 再拷 libeay32.dll / ssleay32.dll（OpenSSL 1.0.1j）
# 4) 压缩目录
```

源码结构：

```
src/           主程序（Qt Widgets）
  protocol.*   LocalSend 协议数据结构与 JSON
  httpserver.* 自写 HTTP/1.1 服务端（TLS、chunked、100-continue、keep-alive）
  httpclient.* 客户端 + 连接复用（keep-alive Session）
  discovery.*  组播发现
  networkservice.*  设备表、扫描、发送/接收编排
  mainwindow.* UI、历史、设置、托盘、玻璃与配色
  ui.*         TaskDialog 封装（Vista+）/ QMessageBox 回退
  i18n.*       内嵌中英文字符串表
icons/         程序图标与传输中图标（ico + 多档 png）
```

---

## 命令行

发布版不依赖命令行，但保留了一些开关，方便排查与自动化测试：

| 开关                                           | 作用                                        |
| ---------------------------------------------- | ------------------------------------------- |
| `--port <n>`                                   | 指定监听端口（**不写入配置**）              |
| `--tray`                                       | 启动后直接进托盘                            |
| `--send <ip[:port]> --file <路径>`             | 无界面发送（文件或文件夹，可重复 `--file`） |
| `--text <文本>`                                | 配合 `--send` 发送一条文本消息              |
| `--auto-accept <目录>`                         | 自动接收（测试用）                          |
| `--auto-reject`                                | 自动拒绝接收（测试用）                      |
| `--scan auto`                                  | 扫描本机各网段并把结果打到日志              |
| `--add-device <ip[:port]>`                     | 启动后自动添加一台设备（用于排查发现失败）  |
| `--force-opaque`                               | 强制不透明外观（模拟无 Aero）               |
| `--ui-test <confirm\|notice\|adddevice\|text>` | 弹一个对话框用于截图                        |
| `--selftest`                                   | 打印样式/字体/合成状态/配置路径后退出       |

环境变量 `LSAERO_FORCE_LEGACY_SCAN=1` 可在新系统上模拟 Windows XP 的扫描档位

---

## 已知限制

* **目前处于 Beta 阶段**：可能仍有小问题；接口与配置键可能调整。
* Windows XP 是**重点照顾**的老平台：为绕开 XP 的半开连接上限与临时端口池，扫描会降级为「一次 8 个连接、每 60 秒一轮」。
  如果 XP 上仍报「无法建立安全连接」，先看 `lsaero.log` 里的 `tls: openssl=… supported=…`：`supported=0` 表示 OpenSSL DLL 没加载（`libeay32.dll` / `ssleay32.dll`必须与 exe 同目录）。
* 组播被严格隔离的网络里，只能靠子网扫描；扫描只探测默认端口 `53317`。
* 语言与字体的切换需要重启程序，本机名称立即生效。
* 文本消息在 LocalSend 1.18.2 上需要对方点一次确认，因此我们最多等 30 秒，超时会提示「对方可能仍在弹窗确认」。
* 接收端默认保存到设定目录，同名文件自动改名，不提供合并/覆盖选项。

---

## 感谢

* **[LocalSend](https://github.com/localsend/localsend)** —— 协议、交互与设计思路的来源。没有这个项目，就没有这份重写。
* **Qt 5.6.3 / MinGW 4.9.2 / OpenSSL 1.0.1j** —— 让「老系统兼容 + 现代 TLS」同时成立。
* 一位叫 **DeepSeek** 的开发者，这个项目几乎全部代码（协议实现、HTTP 服务端、玻璃特效、UI、构建脚本与文档）都是与它在对话中一行行写出来的。
* 一个叫 **控制台MMC** 的开发者提需求、抓 bug 和拍板，以及进行疯狂的软件测试。

---

## 许可证

本项目以 **Apache License 2.0** 开源，见 LICENSE。

第三方组件：

* **Qt 5.6.3** —— LGPLv3（或商业许可）；发布包里附带的是按 LGPL 方式动态链接的 DLL，
  如需替换请使用同名同版本的 Qt 库。
* **OpenSSL 1.0.1j** —— OpenSSL 与 SSLeay 双许可（见 `dist` 内 OpenSSL 版权说明）。

> LocalSend 与 LocalSend 徽标是 LocalSend 项目的商标。本项目仅以「兼容」方式提及它们，
> 不使用其名称作为产品名，也不暗示任何官方认可。

---

## English

**LocalSend Aero** is an unofficial, Qt-based Windows client for the
[LocalSend](https://github.com/localsend/localsend) protocol.

* Native Win32 controls (Common Controls 6.0) instead of a web runtime — runs on
  **Windows XP through Windows 11**, 32-bit, portable, no installer.
* Aero glass is done entirely through **DWM** (`DwmExtendFrameIntoClientArea` +
  per-pixel alpha), so Vista/7 get real blurred glass while 8/10/11 fall back to
  an opaque background automatically.
* Speaks the real LocalSend protocol: multicast discovery with subnet-scan
  fallback, mutual TLS with self-signed certificates and SHA-256 fingerprints,
  chunked / `Expect: 100-continue` uploads, keep-alive, PIN, cancel and text
  messages.
* Verified against **LocalSend 1.17.0**, **1.18.2** and itself — files, folders
  and text messages, in both directions.

Status: **beta**. Licensed under Apache-2.0. Not affiliated with the LocalSend
project.
