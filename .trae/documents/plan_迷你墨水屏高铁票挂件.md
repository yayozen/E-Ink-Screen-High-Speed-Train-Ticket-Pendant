# 迷你墨水屏高铁票挂件 — 实施计划

## 一、项目概述

构建一个由**公网 NAS 上的 Node.js 服务**和 **ESP32-D0WDQ6（V3 版本） + 1.54 寸墨水屏**硬件端组成的小型物联网应用。
- **服务器端**：每日定时（凌晨）调用 12306 抓取本人未来 N 天的车票信息，整理后存入 JSON 文件，对外暴露受 API Key 保护的 HTTP 接口。
- **硬件端**：ESP32-D0WDQ6（V3 版本） 使用纽扣电池供电，通过 RTC 定时器在每天早上固定时间从 deep sleep 唤醒，连接 WiFi → 调用服务器接口拉取最近一张有效车票 → 渲染到 1.54 寸墨水屏 → 再次进入 deep sleep。

## 二、当前状态分析

- 工作目录 `d:\其他项目\迷你墨水屏高铁票挂件\` 为空，全新项目从零开始。
- 已参考 `d:\其他项目\arduino\epaper\` 项目：已具备 1.54 寸墨水屏 + GxEPD2 + U8g2_for_Adafruit_GFX 的驱动模式，但芯片是 ESP32-S3，需要改为 ESP32-D0WDQ6（V3 版本）。
- NAS 上有 Node.js 运行环境（按用户选择），使用 Express/Fastify + JSON 文件存储。

## 三、确认的关键设计决策

| 决策项 | 选择 |
|--------|------|
| 服务器端技术栈 | Node.js (Express/Fastify) |
| 数据源 | 12306 官方（爬虫/接口） |
| 唤醒方式 | 定时唤醒（每日固定时间） |
| 鉴权方式 | 静态 API Key + 设备 ID |
| 数据存储 | JSON 文件存储 |
| 操作系统 | Windows 开发，部署到 NAS（Linux） |

## 四、整体架构

```
┌─────────────────────┐         ┌──────────────────────┐
│   ESP32-D0WDQ6（V3 版本） 挂件     │         │   公网 NAS            │
│  ────────────────   │  HTTPS  │  ──────────────────  │
│  1.54" E-Paper      │ ──────▶ │  Node.js 服务         │
│  RTC 定时唤醒       │ ◀────── │   ├─ 12306 抓票模块   │
│  WiFi STA           │  JSON   │   ├─ JSON 存储        │
│  Deep Sleep         │         │   ├─ HTTP API (Fastify)│
│  CR2032 纽扣电池    │         │   └─ 定时任务 (cron)   │
└─────────────────────┘         └──────────────────────┘
```

## 五、目录结构

```
迷你墨水屏高铁票挂件/
├── server/                          # NAS 端 Node.js 服务
│   ├── package.json
│   ├── .env.example                 # 12306 凭据、API Key 等环境变量示例
│   ├── src/
│   │   ├── index.js                 # Fastify 入口
│   │   ├── config.js                # 配置加载
│   │   ├── auth.js                  # API Key 中间件
│   │   ├── routes/
│   │   │   └── ticket.js            # /api/ticket/next 接口
│   │   ├── services/
│   │   │   ├── cr12306.js           # 12306 抓票/登录
│   │   │   ├── storage.js           # JSON 读写
│   │   │   └── ticketRefresher.js   # 定时刷新任务
│   │   └── utils/
│   │       └── logger.js
│   ├── data/
│   │   ├── tickets.json             # 持久化车票
│   │   └── devices.json             # 已注册设备列表
│   └── logs/
├── firmware/                        # ESP32-D0WDQ6（V3 版本） 固件
│   ├── platformio.ini
│   ├── src/
│   │   ├── main.cpp                 # 主入口：setup + sleep 循环
│   │   ├── config.h                 # WiFi / API Key / 唤醒时间
│   │   ├── wifi_mgr.cpp / .h        # WiFi 连接（NTP 同步）
│   │   ├── http_client.cpp / .h     # HTTP 请求封装
│   │   ├── epaper_render.cpp / .h   # 1.54 寸墨水屏渲染
│   │   ├── rtc_sleep.cpp / .h       # RTC 定时唤醒
│   │   └── json_util.cpp / .h       # ArduinoJson 解析
│   └── README.md
├── docs/
│   ├── API.md                       # HTTP 接口文档
│   ├── 12306抓票说明.md
│   └── 硬件接线.md
└── .gitignore
```

## 六、详细实施步骤

### 阶段 A：服务器端（Node.js + Fastify）

#### A1. 项目初始化
- 在 `server/` 下执行 `npm init -y`；
- 安装依赖：
  - `fastify`（HTTP 框架）
  - `node-cron`（定时任务）
  - `axios` + `https-proxy-agent`（抓取 12306）
  - `tough-cookie` + `axios-cookiejar-support`（Cookie 登录态）
  - `dotenv`（环境变量）
  - `pino-pretty`（日志美化）
- 工具依赖：`nodemon`（开发热重载）、`qrcode-terminal`（首次登录 12306 扫码）。

#### A2. 配置文件 `.env.example`
```
NAS_PORT=3000
DEVICE_API_KEY=replace-with-strong-random-key
CR12306_USERNAME=your_12306_phone_or_email
CR12306_PASSWORD=your_12306_password
REFRESH_CRON=0 3 * * *      # 每天凌晨 3 点抓票
TIMEZONE=Asia/Shanghai
```

#### A3. `src/services/cr12306.js` 12306 抓票模块
- 函数 `login12306()`：
  - 首次扫描二维码登录（控制台输出二维码，扫码后保存 Cookie）；
  - 后续用账密 + 滑块（v2 接口）走自动登录，Cookie 持久化到 `data/cookies.json`。
- 函数 `fetchFutureTickets()`：
  - 请求 `https://kyfw.12306.cn/otn/leftTicket/queryZ` 或 `queryO` 之类的开放接口（视实际可用情况），按身份证号过滤本人车票；
  - 也可以用 `https://kyfw.12306.cn/otn/queryOrder/queryMyOrderNoComplete` 拿到未出行订单做兜底。
- 错误处理：登录态过期 → 重新走扫码登录。
- 输出标准化结构：
  ```json
  {
    "date": "2026-07-04",
    "trainNo": "G1234",
    "fromStation": "上海虹桥",
    "toStation": "杭州东",
    "departTime": "08:15",
    "arriveTime": "09:18",
    "carriage": "12",
    "seat": "08A",
    "ticketStatus": "已出票"
  }
  ```

#### A4. `src/services/storage.js` JSON 存储
- `readTickets()` / `writeTickets(tickets)`：读/写 `data/tickets.json`；
- 用文件锁（`proper-lockfile`）防止并发写入；
- 备份最近一次结果到 `tickets.bak.json` 防损坏。

#### A5. `src/services/ticketRefresher.js` 定时刷新
- 使用 `node-cron` 按 `REFRESH_CRON` 调度；
- 流程：登录态校验 → 抓票 → 标准化 → 写入 JSON；
- 失败重试 3 次，指数退避。

#### A6. `src/auth.js` 鉴权中间件
- 校验请求头 `X-Device-Id` 与 `X-Api-Key`：
  - `X-Device-Id`：ESP32-D0WDQ6（V3 版本） 芯片 MAC 派生字符串；
  - `X-Api-Key`：与 `process.env.DEVICE_API_KEY` 严格相等。
- 不通过返回 401。

#### A7. `src/routes/ticket.js` HTTP 接口
- `GET /api/ticket/next`
  - 从 `data/tickets.json` 读取；
  - 过滤 `date >= today()`；
  - 返回最近一张（按日期升序最早）的车票；
  - 没有则返回 `{ hasTicket: false }`；
  - 响应示例：
    ```json
    {
      "hasTicket": true,
      "ticket": { ...标准化结构... },
      "serverTime": "2026-07-03T18:30:00+08:00"
    }
    ```
- `GET /api/health`：健康检查。

#### A8. `src/index.js` 入口
- 启动 Fastify、注册路由、加载 auth；
- 启动后立即执行一次 `ticketRefresher`；
- 监听 `0.0.0.0:${NAS_PORT}`；
- 优雅退出：监听 SIGINT/SIGTERM。

### 阶段 B：硬件端（ESP32-D0WDQ6（V3 版本） + PlatformIO）

#### B1. `platformio.ini`
```ini
[env:esp32c3]
platform = espressif32
board = ESP32-D0WDQ6（V3 版本）-devkitm-1
framework = arduino
monitor_speed = 115200
lib_deps =
    zinggjm/GxEPD2@^1.5.8
    adafruit/Adafruit GFX Library@^1.11.9
    olikraus/U8g2_for_Adafruit_GFX@^1.8.0
    bblanchon/ArduinoJson@^7.0.4
    esphome/ESPAsyncWebServer@^3.0.0   # 如需本地调试接口
```

#### B2. `src/config.h`
- 集中定义：
  - `WIFI_SSID` / `WIFI_PASS`；
  - `API_HOST`（公网 NAS 域名/IP）；
  - `API_KEY`；
  - `DEVICE_ID`（从 `ESP.getEfuseMac()` 派生）；
  - `WAKE_HOUR` / `WAKE_MINUTE`（默认 07:30）；
  - 1.54 寸墨水屏的引脚映射（参考 arduino/epaper 表，将 S3 引脚改为 C3 等效引脚，例如：CS=5, DC=4, RST=8, BUSY=9, MOSI=6, SCK=7）。

#### B3. `src/wifi_mgr.cpp/.h`
- `connectWifi()`：使用 `WiFi.begin`，超时 15s 失败后直接 deep sleep 下个周期；
- `syncNtp()`：连接 NTP 同步 RTC（`pool.ntp.org`），因为 deep sleep 唤醒后 RTC 还在跑。

#### B4. `src/rtc_sleep.cpp/.h`
- 使用 `esp_sleep_enable_timer_wakeup()` 设置微秒数：
  - `WAKE_HOUR:WAKE_MINUTE` 与当前 RTC 时间差，超 24h 则取模；
  - 用 `esp_deep_sleep_start()` 进入深度睡眠。
- 关键点：ESP32-D0WDQ6（V3 版本） 的 RTC 慢速时钟在 deep sleep 下保持运行，可以做时间戳计算。

#### B5. `src/http_client.cpp/.h`
- 使用 `HTTPClient`（Arduino 内置）；
- 拼接 URL：`{API_HOST}/api/ticket/next`；
- 头：`X-Device-Id`、`X-Api-Key`；
- 读取响应，ArduinoJson 解析。

#### B6. `src/epaper_render.cpp/.h`
- 初始化 `GxEPD2_154_M09 display(cs, dc, rst, busy, SPI);`
- 使用 `U8g2_for_Adafruit_GFX`：
  - 字体 `u8g2_font_unifont_t_chinese1` 或更大号；
  - 字段布局（200x200 像素）：
    - 第 1 行：`{date} {trainNo}`（14pt）
    - 第 2 行：`{fromStation} → {toStation}`（加粗，20pt）
    - 第 3 行：`{departTime} - {arriveTime}`（14pt）
    - 第 4 行：`{carriage}车 {seat}号`（12pt）
    - 第 5 行：`{ticketStatus}`（10pt）
- 无票时显示「今日无票」+ 当前日期 + 时间。
- 显示后调用 `display.hibernate()` 并 `delay(1000)` 等待画面稳定。

#### B7. `src/main.cpp` 主循环
- `setup()`：
  1. 串口初始化；
  2. 连接 WiFi（失败 → 计算下次唤醒时间 → deep sleep）；
  3. 同步 NTP；
  4. 计算当前小时是否在 [WAKE_HOUR-1, WAKE_HOUR+1] 窗口（防止异常时反复唤醒）；
  5. HTTP GET 拉取车票；
  6. 渲染墨水屏；
  7. 关闭 WiFi `WiFi.mode(WIFI_OFF)`；
  8. 计算下次唤醒时间并进入 deep sleep。
- `loop()`：留空。

#### B8. 电源设计要点
- ESP32-D0WDQ6（V3 版本） deep sleep 电流 ~5mA（含 RTC），墨水屏刷新峰值 ~25mA（200ms）；
- CR2032（220mAh）直接驱动 ESP32-D0WDQ6（V3 版本） 不够（峰值 80mA 持续），需要：
  - **方案一（推荐）**：CR2477（1000mAh）+ 钮扣座，或 2 颗 CR2032 并联 + 大容量电容（100uF+）缓冲；
  - **方案二**：直接使用 ESP32-D0WDQ6（V3 版本） 内置 USB 5V 供电 + LDO 降压到 3.3V；
- 在 `硬件接线.md` 给出 2 套方案对比。

### 阶段 C：文档与部署

- `docs/API.md`：接口签名、字段说明、curl 示例、错误码；
- `docs/12306抓票说明.md`：登录流程、Cookie 维护、反爬注意事项；
- `docs/硬件接线.md`：原理图、电池方案、墨水屏引脚对照表、面包板接线图；
- `server/.env.example` 完善说明；
- 在 `firmware/README.md` 说明编译烧录命令：`pio run -t upload`。

## 七、关键文件清单（按实施顺序）

| 序号 | 文件 | 用途 |
|------|------|------|
| 1 | `.gitignore` | 忽略 node_modules、.pio、data/cookies.json、.env |
| 2 | `server/package.json` | 服务端依赖与脚本 |
| 3 | `server/.env.example` | 配置示例 |
| 4 | `server/src/config.js` | 配置加载 |
| 5 | `server/src/utils/logger.js` | 日志工具 |
| 6 | `server/src/services/storage.js` | JSON 读写 + 文件锁 |
| 7 | `server/src/auth.js` | API Key 中间件 |
| 8 | `server/src/services/cr12306.js` | 12306 登录与抓票 |
| 9 | `server/src/services/ticketRefresher.js` | 定时刷新 |
| 10 | `server/src/routes/ticket.js` | /api/ticket/next 路由 |
| 11 | `server/src/index.js` | 启动入口 |
| 12 | `firmware/platformio.ini` | PlatformIO 配置 |
| 13 | `firmware/src/config.h` | 引脚与配置 |
| 14 | `firmware/src/wifi_mgr.*` | WiFi 连接 |
| 15 | `firmware/src/rtc_sleep.*` | 定时唤醒 |
| 16 | `firmware/src/http_client.*` | HTTP 客户端 |
| 17 | `firmware/src/json_util.*` | JSON 解析工具 |
| 18 | `firmware/src/epaper_render.*` | 墨水屏渲染 |
| 19 | `firmware/src/main.cpp` | 主入口 |
| 20 | `docs/API.md` | 接口文档 |
| 21 | `docs/12306抓票说明.md` | 抓票说明 |
| 22 | `docs/硬件接线.md` | 接线文档 |

## 八、依赖与约束

- 服务器需要 Node.js ≥ 18；
- 12306 接口不稳定：实现要带重试和容错，并允许运营期间手动触发刷新（`POST /api/refresh`）；
- 12306 强反爬：建议加 `User-Agent` 伪装、随机延时、必要时使用住宅代理 IP；
- 1.54 寸墨水屏分辨率 200x200，中文渲染需用 `u8g2_font_unifont_t_chinese1`（占用 Flash 较大，注意 ESP32-D0WDQ6（V3 版本） 的 4MB Flash 限制）；
- API Key 必须保密，建议 `openssl rand -hex 32` 生成。

## 九、验证步骤

### 服务器端
1. `npm run dev` 启动，看到 `Server listening on 0.0.0.0:3000`；
2. `curl http://localhost:3000/api/health` → 返回 `{"status":"ok"}`；
3. `curl -H "X-Device-Id: test" -H "X-Api-Key: <key>" http://localhost:3000/api/ticket/next` → 返回有效 JSON；
4. 错误 API Key → 401；
5. 看 logs 确认 cron 在凌晨 3 点执行了抓票。

### 硬件端
1. `pio run -t upload` 烧录固件；
2. 串口监视器看到 WiFi 连接成功 → HTTP 200 → 墨水屏刷新；
3. 等待 1 分钟：墨水屏保持画面（不耗电）；
4. 等待到设定唤醒时间：设备自动唤醒、刷屏、再次睡眠；
5. 拔掉 USB 后使用纽扣电池供电测试 48h 续航。

### 端到端
1. 在 12306 模拟购买一张明日车票；
2. 等待服务器定时刷新或手动触发；
3. 次日早上手动按键或定时唤醒后，墨水屏显示该车票信息。

## 十、假设与未决项

- **未决 1**：12306 抓取接口需要有效登录态，用户需提供 12306 账号/二维码登录（**已确认方案是 12306 官方接口**）。
- **未决 2**：电池方案尚未确定，文档中将给出 2 套推荐方案供选择。
- **未决 3**：墨水屏中文显示字体占用较大，ESP32-D0WDQ6（V3 版本） 默认 4MB Flash 中约 1MB 给 SPI Flash 分区，需要在 `platformio.ini` 中合理设置 `board_build.partitions`（如 `min_spiffs.csv`）。
- **未决 4**：首次部署需 NAS 已开放公网端口（3000 或反代到 443），并配置 HTTPS 证书（建议用 Caddy/Nginx 反代）。
