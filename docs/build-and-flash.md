# WatchDog 构建 / 烧录 / 排障手册

> 把项目踩过的坑和已验证的正确做法记录下来，避免重蹈覆辙。

## 0. 项目速览

| 项 | 值 |
|---|---|
| 固件源码 | `WatchDog/WatchDog.ino`（零第三方库，约 22KB） |
| 网页文件 | `WatchDog/data/`（index.html / setup.html / files.html / ui.css / ui.js / config.txt） |
| 编译产物 | `固件/WatchDog.bin`（约 497 KB，v1.3.2） |
| 文件系统镜像 | `固件/filesystem.bin`（1,024,000 B，-b 8192 -p 256 -s 0xFA000） |
| 烧录脚本 | `工具/flash.ps1` |
| 设备 | WeMos D1（ESP8266MOD，4MB flash） |
| mDNS | hostname `watchdog` → watchdog.local |

> `固件/` 目录不随仓库分发（内含个人 WiFi 凭据 / SendKey，勿提交）。
> 自行编译后放入该目录，或烧录时用 `-Bin` 指定。

---

## 1. 最大历史坑：LittleFS 挂载失败（表现为“网页打不开 / 一直进配网模式”）

**症状**：`http://watchdog.local` 打不开，设备一直进 WatchDog-Setup 配网模式，`/api/config` 读不出配置。

**根因**：打包文件系统的 `mklittlefs` 参数与固件期望不一致。

- 固件（esp8266 core 3.0.2，4M1M 布局）期望：
  - block_size = **8192**
  - page = **256**
  - FS 起始地址 = **0x300000**
  - FS 大小 = **0xFA000**（1,024,000 B）
- 错误做法：用 `mklittlefs` 默认值打包（block=4096）→ 镜像虽生成、烧进去也不报错，但固件挂载 LittleFS 静默失败 → config.txt 读不出 → 误以为没配置 → 进 AP 配网模式。

**正确打包命令**：
```powershell
cd 工具
.\mklittlefs.exe -c ..\WatchDog\data -b 8192 -p 256 -s 0xFA000 ..\固件\filesystem.bin
```
验证打包结果：
```powershell
.\mklittlefs.exe -l -b 8192 -p 256 -s 0xFA000 ..\固件\filesystem.bin
# 应列出 6 个文件：config.txt files.html index.html setup.html ui.css ui.js
```
镜像前 4 字节应为 littlefs v2 魔数 `01 00 00 00`（superblock）。

**烧录地址（固定不变）**：
- 固件 → **0x0**
- 文件系统 → **0x300000**
- flash 模式必须 `--flash_mode dout --flash_size 4MB`

```powershell
python -m esptool --port COMx --baud 460800 write_flash --flash_mode dout --flash_size 4MB `
  0x0 "..\固件\WatchDog.bin" 0x300000 "..\固件\filesystem.bin"
```
烧完必须看到两段都 `Verifying written data... Hash of data verified.`

---

## 2. 大坑：GitHub 直连下载大文件全部失败（curl exit 35 / http=000）

**症状**：`curl.exe` 下载 GitHub releases 的大文件（core、编译器 zip）**0.01 秒即断**：`exit 35`（SSL 握手失败）、`http=000`、`size=0`。

**关键判别（别白费时间）**：
- 同一 URL 用 `Invoke-WebRequest -Method Head` 返回 **200** → 网络通，问题只在 **curl.exe 对 GitHub 下载通道（objects.githubusercontent.com 重定向）的 TLS**。

**正确做法（已验证）**：
1. 源码/仓库用 git 浅克隆（git 协议连 github.com 是通的）：
   ```powershell
   git clone --depth 1 --branch 3.0.2 https://github.com/esp8266/Arduino <目录>
   ```
2. 二进制（工具链等）用 GitHub 加速镜像（gh-proxy.com 已实测可用，大小与官方一致）：
   ```
   https://gh-proxy.com/https://github.com/<user>/<repo>/releases/download/...
   ```
   备用镜像：`https://ghfast.top/https://github.com/...`（200 验证过）。

---

## 3. 大坑：esp8266 core 3.0.2 是 git 仓库 + submodule，浅克隆必须补 submodule

**症状**：`arduino-cli compile` 报：
```
libraries/LittleFS/src/LittleFS.h:38:10: fatal error: ../lib/littlefs/lfs.h: No such file or directory
```
**原因**：core 仓库的 `libraries/LittleFS/lib/littlefs`、`tools/sdk/ssl/bearssl`、`tools/esptool`、`tools/pyserial`、`tools/sdk/lwip2/builder` 等目录是 **git submodule**，`--depth 1` 浅克隆不会自动拉取。

**正确做法**：
```powershell
git -C <esp8266-src 目录> submodule update --init --recursive
```
验证关键文件：
- `esp8266-src\libraries\LittleFS\lib\littlefs\lfs.h`
- `esp8266-src\tools\sdk\ssl\bearssl\inc\bearssl.h`

---

## 4. 坑：arduino-cli `core install` 会卡死，改用“手动安装平台”

**症状**：`arduino-cli core install esp8266:esp8266@3.0.2` 加索引后长时间无进展。

**正确做法（手动安装，已验证）**：
1. 下载 `arduino-cli.exe`（downloads.arduino.cc 可直连）：`https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip`
2. 下载 4 个工具（URL 从 `%LOCALAPPDATA%\Arduino15\package_esp8266com_index.json` 解析，取 **host=x86_64-mingw32** 条目；全部走加速镜像）：

   | 包 | 版本 | 大小 |
   |---|---|---|
   | xtensa-lx106-elf-gcc | 3.0.4-gcc10.3-1757bed | 75 MB |
   | python3 | 3.7.2-post1 | 6.4 MB |
   | mkspiffs | 3.0.4-gcc10.3-1757bed | 350 KB |
   | mklittlefs | 3.0.4-gcc10.3-1757bed | 345 KB |

   （core 本体不用下载 zip —— git 克隆的 esp8266-src 就是完整 core。）
3. 解压，按平台目录约定放置：
   - `esp8266-src` → `%LOCALAPPDATA%\Arduino15\packages\esp8266\hardware\esp8266\3.0.2\`（**先补 submodule 再复制！**）
   - gcc zip 内顶层 `xtensa-lx106-elf\` → 平台目录 `tools\xtensa-lx106-elf\`
   - python3 zip 内顶层 `python3-3.7.2.post1-embed-win32\` → `tools\python3\`
   - mkspiffs / mklittlefs → `tools\mkspiffs\`、`tools\mklittlefs\`
4. 验证 `arduino-cli core list` 出现 `esp8266:esp8266  3.0.2`。

> 注：GIT 版 platform.txt 的工具路径是**平台自包含**的（`{runtime.platform.path}/tools/xtensa-lx106-elf` 等），所以只要平台目录内工具齐全就能编译，不依赖 Arduino15 的 packages/tools 元数据。

---

## 5. 编译命令（固定参数，别改）

```powershell
$env:TEMP = "<可写临时目录>"; $env:TMP = $env:TEMP   # 默认 Temp 目录可能无写权限，先改
& "<arduino-cli.exe 路径>" compile `
  --build-path "<工作区目录>" `
  -b "esp8266:esp8266:generic:xtal=80,vt=flash,exception=disabled,stacksmash=disabled,ssl=all,mmu=3232,non32xfer=fast,ResetMethod=nodemcu,CrystalFreq=26,FlashFreq=80,FlashMode=dout,eesz=4M1M,led=2,sdk=nonosdk_190703,ip=lm2f,dbg=Disabled,lvl=None____,wipe=none,baud=115200" `
  "WatchDog\WatchDog.ino" --output-dir "<输出目录>"
```

**关键参数含义**（改错会烧出布局不对的固件）：
- `FlashMode=dout`：与烧录 `--flash_mode dout` 一致
- `eesz=4M1M`：4MB flash、1MB 文件系统分区（对应 FS 地址 0x300000、大小 0xFA000）
- `FlashFreq=80`：80MHz
- `ssl=all`：必须（固件用 WiFiClientSecure 走 HTTPS）

**注意**：`--output-dir` 生成的固件名是 `WatchDog.ino.bin`，需改名 `WatchDog.bin`（flash.ps1 按此名找文件）。

当前编译基线（v1.3.x）：Flash 约 507 KB（48%）、RAM 约 41 KB（50%）。

---

## 6. 常用维护命令速查

| 要做的事 | 命令 |
|---|---|
| 只改网页后重烧 FS | `python -m esptool --port COMx write_flash --flash_mode dout --flash_size 4MB 0x300000 "固件\filesystem.bin"`（先重新 mklittlefs 打包） |
| 抓 boot 日志 | 复位设备后串口工具 @115200 |
| 找设备 IP | `.\工具\打开看板.ps1 -NoOpen`（先试 mDNS，再按 MAC 扫 ARP） |
| 测试推送 | `POST http://<设备IP>/api/testpush` → `{"ok":true}` |
| 查看配置 | `GET http://<设备IP>/api/config` → 含 `"has_sendkey":true/false` |

---

## 7. 环境杂项（容易再踩）

- PowerShell 输出经常被吞 → **重定向到文件再 `Get-Content` 读**。
- PowerShell 5.1 无 heredoc，python 脚本先写成 `.py` 再执行。
- `arduino-cli compile` 必须加 `--build-path <工作区目录>` 且 `TEMP`/`TMP` 指向可写目录，否则报 `Access is denied`。
- **千万别用 PowerShell 文本管道改源码**：`(Get-Content x.ino -Raw) | Set-Content x.ino -Encoding utf8` 会把无 BOM 的 UTF-8 中文按 GBK 读进来、再当 UTF-8 写回去，**不可逆地毁掉全部中文**。改文件用编辑器；要批量替换就写 Python 脚本，`open(p,"rb").read().decode("utf-8")`。
- `.ps1` 脚本必须存成 **UTF-8 with BOM**，否则 PowerShell 5.1 按 ANSI 读，会报“字符串缺少终止符”。
- 设备 USB 掉线症状：串口“设备没有发挥作用” → 重新插拔 USB。

---

## 8. 网页“打不开”排障（v1.1.0 已修）

**现象**：浏览器打开设备 IP 一直转圈，偶尔白屏；串口上看设备有时干脆卡在配网模式。

### 排查链（按顺序做，每步都有判据）

1. **先看串口，别猜**：复位后抓 115200 日志。
   - 出现 `[fs] mount failed, formatting...` → 才是 §1 的 FS 问题。
   - 出现 `[sta] connecting <ssid>` 却始终没有 `ip=` → 是 WiFi 关联问题。
   - `[setup] AP WatchDog-Setup` → 说明已退回配网模式，只能连它自己的热点。
2. **确认设备到底在不在线**：ping/curl 前先确认 IP。**DHCP 会换地址**，用 `http://watchdog.local`，或按 MAC 在 `arp -a` 里找。
3. **用字节数判“真打开”**：`curl -s -o NUL -w "%{size_download}" http://<ip>/` 对比 `data/index.html` 的大小。拿到几百/几千字节 = 分块发送被截断。
4. **空闲后第一次请求最致命**：空闲 45 秒再请求 `GET /`，应 1~3 秒返回完整页面。

### 三个根因（都在固件侧）

| # | 根因 | 表现 |
|---|---|---|
| A | **WiFi 自动省电（modem sleep）没关** | 空闲后第一包卡 5~40 秒，浏览器超时 → “打不开” |
| B | `File::sendAll()` / `client.write()` 超时截断后**不重发** | Content-Length 完整，实际只出去几百~几千字节 → 白屏 |
| C | 20 秒硬超时 + 只 `begin()` 一次 | 弱信号关联要 20~40 秒，误判成连不上 → 退回配网模式 |

### 修法（`WatchDog.ino` 里已落地）

- `onStaConnected()` 里 `WiFi.setSleepMode(WIFI_NONE_SLEEP)`。
- `serveFile()` 不再用 `streamFile()`：改成 `sendFileSafe()` —— `setContentLength()` + **512 B 分块**，短写（`write()` 返回 0）就 `delay(2)` 重发，25 秒写不动才放弃，收尾 `client.flush()` 把 TX 队列排空。
- `startSta()`：开机先 `disconnect(true)` + `scanNetworks()`（预热射频**并拿到目标 AP 的 BSSID/信道**）→ `begin(ssid, pass, ch, bssid)` 直连；45 秒预算内每 12 秒重发一次，前 3 次带 BSSID，之后退回普通 `begin()`。
- 配网模式不再“等死”：`loop()` 里每分钟再试一次，连上就 `softAPdisconnect` 自动退出 AP 模式。

---

## 9. 版本演进记录（实现要点）

### v1.1.0
- 修 §8 三个根因；`FW_VERSION` → 1.1.0。

### v1.2.0（健康页 / 断网落盘 / 主备 WiFi / 日报）

| 功能 | 落地方式 |
|---|---|
| 健康/复位卡片 | `GET /api/health`，看板「设备健康」卡 |
| heap 看护 | heap < 6KB 持续 5 分钟 → 记录后自动重启（`HEAP_GUARD_MIN`） |
| 断网事件落盘 | `/outages.bin`，40 条环形，328 B |
| 分层断网判定 | 网关不通=局域网断，网关通=外网断；`status.err`：1=外网断 2=局域网断 |
| p95 / 抖动 / max | `/api/status` 里 `p50/p95/max_rtt/jitter` |
| CSV 导出 | 看板「导出 CSV」→ `/api/history` |
| 主备 WiFi | `ssid2/pass2`，开机扫一趟抓两组的 BSSID+信道，逐个试 |
| 每日简报 | 每天 08:00 推 Server 酱（可用率/断网次数/p95/信号），`/rep.day` 去重 |

**关键实现点（下次改动别破坏）**：
1. **写盘时机**：断网开始写一条 `dur=0` 的“进行中”记录，恢复时补时长；开机 `outagesLoad()` 发现最后一条 `dur==0` 就 `inOutage=true` 接着算 —— 断网中途断电也不丢整条。
2. **写放大**：一次断网只写 2 次（开始 + 结束），整个文件 328 B 覆盖写。
3. **日报去重**：`/rep.day` 存 `年*1000+yday`，先写文件再推，避免重启后重复推送。
4. **Arduino 原型注入**：`scanTargets(ApTarget*, ...)` 这种“参数带自定义结构体”的辅助函数编译不过（原型会被插到 `#include` 之后、结构体之前），已改成在 `startSta()` 里内联循环。
5. **刷文件系统会清数据**：`flash.ps1 -FsOnly` 是整片替换 0x300000，`/hist.bin`、`/outages.bin`、`/rep.day` 全部重置。只想改网页而不想丢数据，用 `POST /api/ota` 或单独写不覆盖这些文件的镜像。

### v1.3.0（固定 IP + 清空记录）

- 配置加 `ip/gw/mask`，留空仍走 DHCP；`GET /api/clear?what=outages|hist|all` 清空记录（连文件一起删）。

**大坑：WiFi.config() 会被 disconnect/mode 清掉**
第一版把 `applyStaticIp()` 放在 `startSta()` 开头，后面紧跟着 `WiFi.disconnect(true); WiFi.mode(WIFI_STA);` —— 这两步会把静态 IP 配置抹掉，但 `WiFi.localIP()` **仍然返回那个静态地址**，于是日志出现极具迷惑性的 `connected '' ip=<静态地址> rssi=正数 ch=1`。
**正确做法：`WiFi.config()` 必须在每次 `WiFi.begin()` 之前立刻调用**（现在放在 `trySta()` 里 begin 前面）。

判别口诀：
- `SSID` 空 + `rssi>0` + `ch=1` + `gw_rtt=0` → 配置没生效，不是信号问题
- 静态 IP 设完必须从另一台机器 `curl -o NUL -w "%{http_code}"` 确认 200，别只看串口日志

### v1.3.1（配置保存的坑：改 SendKey 却把 WiFi 密码清空）

**现象**：网页上只改了 SendKey，保存重启后设备连不上 WiFi。

**根因**：旧代码对密码字段**无条件赋值**，而网页出于**安全从不回显密码**，输入框永远是空的；保存时 JS 把所有输入框都提交，于是空串 = 把密码/密钥清空。

**修法**：密码/密钥类字段一律“留空 = 不修改”，要清空必须走显式开关：
```cpp
if (server.hasArg("pass")    && server.arg("pass").length())    cfg.pass    = server.arg("pass");
if (server.hasArg("pass2")   && server.arg("pass2").length())   cfg.pass2   = server.arg("pass2");
if (server.hasArg("webpass") && server.arg("webpass").length()) cfg.webpass = server.arg("webpass");
if (server.hasArg("sendkey") && server.arg("sendkey").length()) cfg.sendkey = server.arg("sendkey");
if (server.hasArg("webpass_clear")) cfg.webpass = "";
if (server.hasArg("sendkey_clear")) cfg.sendkey = "";
```

> 教训：**任何“保存后重启”的表单，密码类字段必须留空=不修改**；这条对以后加的任何 token/密钥都成立。

### v1.3.2（网页密码 HTTP Basic）

- 用户名固定 `admin`，密码 = 设置页「网页密码」（`config.txt` 的 `webpass=`）；`webpass=` 为空 → 不验证。
- realm 改成 `WatchDog (username: admin)`，浏览器弹框自己会显示用户名。
- **忘了密码的救法**：① 重烧文件系统 `flash.ps1 -FsOnly`（`data/config.txt` 原样写回，`webpass=` 为空 = 密码直接没了；代价是 `/hist.bin`、`/outages.bin` 一起清）；② 浏览器试 `admin` + 常用密码。

**安全提醒**：HTTP Basic 是**明文**传输，同网段抓包就能看到。内网自用可以，别在公共 WiFi 下开这个页面；真要讲究就上反代 + HTTPS。

**安卓 Chrome 的 `ERR_HTTP_RESPONSE_CODE_FAILURE`**：网页密码开着时，手机第一次访问 `/` 会拿到 `401`，安卓 Chrome 常常不弹登录框而是直接报错。处理：① 页面设置勾「清除网页密码」；② 手机 Chrome → 设置 → 站点设置 → 清除该站点数据。另外 `401` 响应 body 为空（`Content-Length: 0`），以后要在手机端用认证，至少得给 401 配一个 HTML body。
