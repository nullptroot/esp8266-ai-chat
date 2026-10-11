# esp8266-ai-chat

ESP8266 单文件固件：上电即开热点，网页选 WiFi，连入局域网后在网页上直接与远端
OpenAI 兼容模型 API 对话，并支持用自然语言让 AI 控制板子引脚（高/低电平、
保持时长、闪烁、LED 状态）。全部设置（WiFi、模型 API 地址 / Key / 模型名）
在网页上配置并持久化到 flash，断电不丢、重烧录不丢。

## 功能

- **上电即热点**：SoftAP `ESP8266-Web`（密码 `12345678`，IP `192.168.4.1`），
  始终可用，兼做回退（Station 连不上也不会失联）。
- **网页选 WiFi**：扫描 2.4GHz 列表 → 选一个输密码连接（15s 超时，失败自动
  回退热点）。选择持久化，断电重连自动生效。
- **AI 对话**：连入局域网后，网页提供简单对话窗口，直连远端 OpenAI 兼容
  `/v1/chat/completions`（已实测 llama.cpp + Qwen3.8-27B，多轮上下文）。
- **引脚控制（AI + 网页）**：AI 对话里说「让引脚 5 高电平持续 2 秒」「让 LED
  闪烁」，模型按约定协议输出 `[PIN]{...}[/PIN]` 指令，固件解析执行并在回复中
  反馈结果；网页 ③ 面板也可直接操作（高/低/闪烁/关闭 + 间隔 + 持续时长）。
  详见下文「引脚控制协议」。
- **模型 API 可配置**：地址 / Key / 模型名网页可改，持久化到 flash 的
  EEPROM 仿真区（`0xFB000`，4KB，位于 app 镜像之后，`write_flash 0x0` 不会碰它）。
- **LED 状态**：GPIO2 心跳 1Hz（页面可切常亮 / 常灭）。
- **mDNS**：`esp8266-web.local`。

## 硬件

- ESP8266EX 模块（ESP-01 一类，1MB 及以上 flash；本机在 4MB flash 模块上
  用 1MB 布局 `esp01_1m` 验证通过）。
- CP210x / CH340 USB 转串口（任一）。

## 编译

需要 [PlatformIO](https://platformio.org)（建议独立 venv）：

```bash
cd firmware
export PLATFORMIO_CORE_DIR=$PWD/.pio
export PLATFORMIO_SYSTEM_CACHE_DIR=$PWD/.pio/cache
pio run
```

- platform：`espressif8266@4.2.1`，framework：`arduinoespressif8266@3.30102.0`（自动安装）
- 依赖：`bblanchon/ArduinoJson@6.21.4`（`lib_deps` 自动拉取）
- 产物：`firmware/.pio/build/esp8266/firmware.bin`（PlatformIO env 名是 `esp8266`，board 才是 `esp01_1m`）

参考占用：RAM ≈ 40%（32.9KB/81.9KB），Flash ≈ 48%（363KB/744KB）。

## 烧录

```bash
esptool.py --port /dev/ttyUSB0 --baud 460800 write_flash 0x0 firmware.bin
```

## 串口抓取（注意 DTR 坑）

ESP8266 在 DTR 拉低时会进下载模式，普通串口监视器一打开就可能把芯片拉挂。
用本仓库自带的脚本（会先 `setDTR(False)` / `setRTS(False)`）：

```bash
pip install pyserial
python3 tools/monitor_app.py /dev/ttyUSB0 115200 30 boot.txt
# 用法: monitor_app.py [串口] [波特率] [秒数] [输出文件] [--no-reset]
```

启动成功的标志是串口里出现 `http://<IP>/`（STA 连上网后打印）。

## 网页 API

| 路径 | 方法 | 说明 |
|---|---|---|
| `/` | GET | Web 控制台（状态 / WiFi 选择 / AI 对话 / 引脚控制 / 模型配置） |
| `/api/status` | GET | 运行状态：IP、RSSI、heap、uptime、LED 模式 |
| `/api/led?mode=0\|1\|2` | GET | LED 心跳 / 常亮 / 常灭 |
| `/api/pin` | POST | 引脚控制；参数 `pin`、`action=high\|low\|blink\|off\|heartbeat`、`interval_ms`（闪烁间隔）、`duration_ms`（到时自动恢复低电平） |
| `/api/pins` | GET | 全部 17 个引脚当前状态（idle/high/low/blink:间隔ms/heartbeat） |
| `/api/wifi/scan` | GET | 2.4GHz 扫描列表（阻塞约 10s，去重 ≤15 条） |
| `/api/wifi/connect` | POST | 连接指定 SSID（返回 202，15s 超时） |
| `/api/wifi/disconnect` | POST | 断开 Station |
| `/api/chat` | POST | AI 对话；参数 `history` = messages JSON 数组（最多 20 条） |
| `/api/chat/config` | GET/POST | 读 / 写模型 API 配置（`url`/`key`/`model`），写后持久化 |

## 引脚控制协议

**AI 对话控制**：固件在转发给模型的 messages 数组最前面注入一条协议系统消息，
要求模型在需要控制引脚时输出一行控制指令：

```
[PIN]{"pin":5,"action":"high","duration_ms":2000}[/PIN]
```

固件解析执行，把指令行从回复里去掉，并追加一行结果反馈（`⚡ 已执行: …` 或
`⚠️ …`），例如：

> 用户：让引脚 5 高电平持续 2 秒
> 模型：已将引脚 5 设置为高电平并保持 2 秒。
> ⚡ 已执行: 引脚5 高电平 (约 2000 ms 后自动恢复低电平)

指令字段：

| 字段 | 说明 |
|---|---|
| `pin` | 引脚号 0~16（实际可用见下表） |
| `action` | `high` 高电平 / `low`、`off` 低电平 / `blink` 闪烁 / `heartbeat` 恢复 LED 心跳（仅 pin 2） |
| `interval_ms` | 闪烁翻转间隔，缺省 500，范围 20~30000 |
| `duration_ms` | 保持时长（毫秒），到时自动恢复低电平；用户没说时长则省略 |

解析容错：模型没带 `[PIN]` 标记时，固件会兜底抓取回复里第一个同时含
`"pin"` 和 `"action"` 的 JSON 对象尝试执行。

**ESP-01 引脚可用性**（固件内置策略，不可用的引脚 AI 和网页都会被拒绝并说明原因）：

| 引脚 | 状态 | 原因 |
|---|---|---|
| 2 | ✅ 板载 LED | 低电平点亮；心跳 / 常亮 / 常灭 / 闪烁 |
| 3 / 4 / 5 | ✅ 可用 GPIO | |
| 15 | ✅ 可用 GPIO | |
| 16 | ✅ 只能输出 | RTC 引脚，不能读 |
| 0 | ❌ 锁定 | flash 片选 CS，驱动低电平会关掉 flash 使芯片崩溃 |
| 1 | ❌ 锁定 | UART TXD，动了就废掉串口输出 |
| 6~14 | ❌ 锁定 | flash 数据/时钟引脚，或本模块未引出 |

> 如果你的模块引出了更多引脚（如 ESP-12F），改 `main.cpp` 顶部的
> `PIN_OK[]` / `PIN_NOTES[]` 两个数组即可（注意 0/1 永远不要放开）。

## 使用

1. 上电 → 手机 / 电脑连热点 `ESP8266-Web`（密码 `12345678`）。
2. 打开 `http://192.168.4.1/`。
3. 在页面扫描并选择 2.4GHz WiFi → 输密码连接。
4. 连上后设备取得局域网 IP（串口会打印 `http://x.x.x.x/`），之后直接访问该 IP。
5. 填写模型 API，例如 llama.cpp：
   ```bash
   ./llama-server -m model.gguf --port 8000 --api-key anystring
   ```
   - 地址：`http://<模型服务器IP>:8000/v1`
   - Key：`anystring`（与 `--api-key` 一致）
   - 模型：模型文件路径（llama.cpp 的约定）
6. 页面上开始对话。
7. 对话里直接用自然语言控制引脚（也可用 ③ 面板点按钮）：
   - 「让引脚 5 高电平持续 2 秒」→ 固件执行并在回复中反馈 `⚡ 已执行: …`，2 秒后自动恢复低电平
   - 「让 LED 闪烁」「LED 常亮」「关闭 LED」「恢复 LED 心跳」→ 板载 LED 各状态
   - 「让引脚 1 高电平」→ 会被拒绝并说明原因（0/1/flash 引脚被固件锁定）

> 本仓库出厂默认值全部留空（不内置任何 WiFi 密码 / API Key / 内网地址），
> 首次上电请按上面步骤在网页里填好，之后自动持久化。

## 已知的坑（代码注释里也有，务必保留）

- `board=esp01_1m`（variant generic）把 `LED_BUILTIN` 定义为 **1**，而 GPIO1
  就是 UART TXD——引用 `LED_BUILTIN` 会点亮串口、彻底打乱串口输出。
  本固件只用 GPIO2。
- 源文件必须是 `src/main.cpp`：`.ino` 预处理器会把函数原型插进跨行原始字符串
  （整段网页 JS）里，编译不报错但页面全废。
- 该 framework 版本（3.30102.0）没有 Preferences 库；持久化用 framework 自带
  的 EEPROM 仿真库。
- 该 framework 的 `HTTPClient::begin(const char*)` 是**硬编译错误**
  （obsolete API），必须用 `begin(WiFiClient&, url)`。
- ArduinoJson 6.x：`JsonDocument` 是抽象基类，要用 `DynamicJsonDocument`。
- `loop()` 里禁止长 `delay()`（阻塞 HTTP 服务）。

## 目录

```
firmware/
  platformio.ini
  src/main.cpp      # 全部固件（单文件）
tools/
  monitor_app.py    # 串口抓取脚本（处理 DTR 坑）
```
