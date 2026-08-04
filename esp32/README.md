# Firmware - 迷你墨水屏高铁票挂件

ESP32-D0WDQ6（V3 版本） + 1.54 寸墨水屏固件。

> 当前版本：**固件端直接 IMAP 抓票**。无需任何 NAS/服务端，ESP32 唤醒后连 WiFi → 登录 126 邮箱 → 拉取 12306 购票通知邮件 → 解析 → 刷墨水屏。

## 编译与烧录

使用 [PlatformIO](https://platformio.io/)：

```bash
# 编译
pio run

# 烧录
pio run -t upload

# 串口监视器
pio device monitor
```

首次烧录前请修改 `src/config.h` 中的：
- `WIFI_SSID` / `WIFI_PASS`
- `IMAP_USER`（126 邮箱地址）
- `IMAP_PASS`（126 邮箱 IMAP 授权码，非登录密码）
- `WAKE_HOUR` / `WAKE_MINUTE`（每日唤醒时间）

## 目录结构

```
firmware/
├── platformio.ini         # PlatformIO 配置
└── src/
    ├── main.cpp           # 主入口
    ├── config.h           # 全局配置与引脚定义
    ├── wifi_mgr.h/.cpp    # WiFi 连接 + NTP 同步
    ├── rtc_sleep.h/.cpp   # RTC 定时 deep sleep 唤醒
    ├── mail_imap.h/.cpp   # 126 IMAP 客户端（手写协议）
    ├── mail_parser.h/.cpp # 12306 纯文本邮件解析
    ├── ticket_enrich.h/.cpp # 检票口 / 到达时间外部补全（12306 接口）
    ├── epaper_render.h/.cpp # 1.54 寸墨水屏渲染
    └── json_util.h        # 车票数据结构定义
```

## 工作流程

```
BOOT → 初始化墨水屏 → 连接 WiFi → 同步 NTP
     → IMAP SSL 连接 imap.126.com:993
     → LOGIN → SELECT INBOX
     → UID SEARCH SINCE <15daysAgo> FROM "12306@rails.com.cn"
     → UID FETCH ... BODY[TEXT] （只取纯文本）
     → 解析为 MailTicket（带 kind 标记：购票/退票/改签/候补）
     → 倒序遍历，按业务规则筛选
     → 调 12306 接口补全检票口 / 到达时间（按需）
     → 渲染墨水屏
     → 关闭 WiFi
     → deep sleep 等待次日 WAKE_HOUR:WAKE_MINUTE
```

## 挂件图与静态挂件模式

设备支持通过 BLE 配网页面上传一张 1bit 挂件图（250×122 / 212×104），存入 NVS（key=`pb_img`，不加密）。挂件图有两个显示场景：

1. **无票 fallback**：服务端无有效车票时（`needUpdate=true + hasTicket=false`），显示挂件图代替 "NO TICKET" 文本
2. **缺配置静态挂件**：未配置 IMAP/WiFi 等抓票信息时，设备显示挂件图后进入永久 deep sleep（不设唤醒源，仅 RST/重新上电可唤醒），当作可自定义图片的静态挂件

### 静态挂件用法

1. 烧录固件后上电，冷启动开 15s BLE 窗口
2. 用浏览器打开 [docs/cfg.html](../docs/cfg.html) 连接设备，上传挂件图（浏览器端自动缩放 + Atkinson 抖动为 1bit）
3. 不配置 IMAP / WiFi / 唤醒时间
4. BLE 窗口超时后，设备检测到缺配置 → 显示挂件图 → 永久 deep sleep
5. 想换图时按 RST 重启，重新进入 15s BLE 窗口上传

> 注：缺配置时若 NVS 无挂件图，则显示英文提示 `Config required! / Restart, config in 15s / BLE: TicketBadge-Cfg` 后永久睡眠。

## 抓取策略（满足三条用户要求）

| 要求 | 实现 |
|------|------|
| 邮件取纯文本正文 | `UID FETCH <uid> BODY[TEXT]`，不下载 HTML/附件 |
| 仅发件人为 12306@rails.com.cn | `SEARCH SINCE <d> FROM "12306@rails.com.cn"` |
| 最近 15 天 | `IMAP_FETCH_DAYS=15`，由 NTP 时间生成 `SINCE 24-Jun-2026` |

为避免极少数情况下邮件量过大，单次最多处理 50 封。

## 业务筛选规则（与 PC 端 `mail-fetcher/src/ticket-finder.js` 保持一致）

抓取到的邮件按"时间正序 UID 升序"放入数组，**倒序遍历**：

1. **退票通知**（正文含"已退票/已退款/退票成功/退票通知"）：仅记录其订单号到 `refundOrderNos`，不参与选票；倒序走到原购票时按订单号跳过。
2. **改签通知**（正文含"改签"+"次列车"）：算一条有效购票记录，同时把订单号记到 `changeOrderNos`；倒序走到原订单时跳过。
3. **候补订单兑现**（正文含"候补"+"次列车"）：算一条有效购票记录。
4. **普通购票通知**（正文含"次列车"且无退票关键字）：若订单号已记录在退/改集合中则跳过。
5. **过期过滤**：`date + departTime`（北京时间）转 unix 毫秒后，与 NTP 同步后的 `time()` 比较；过期车票跳过。
6. **取日期最小**：在所有有效车票中取"出行日期最小"的一张。

邮件类型由 `mail_parser.cpp` 中的 `classifyBody()` 按正文关键字判定，并把 `MailKind` 写入 `MailTicket.kind`。

## 外部字段补全（12306 接口）

业务筛选确定 `out.ticket` 后，对邮件正文中可能缺失的两个字段做按需补全：

| 字段 | 缺失时调用 | 接口模板（在 `config.h`） |
|---|---|---|
| `checkPosition` | `enrichCheckPosition()` | `mobile.12306.cn/weixin//wxcore/getPlatform?trainCode=...&stationName=...&date=...` |
| `arriveTime` | `enrichArriveTime()` | 两步：`search.12306.cn/search/v1/train/search?keyword=...&date=YYYYMMDD&type=wx_checi` → `mobile.12306.cn/weixin/wxcore/queryByTrainNo?train_no=...&from_station_telecode=BBB&to_station_telecode=BBB&depart_date=YYYY-MM-DD` |

实现细节见 [ticket_enrich.h](src/ticket_enrich.h) / [ticket_enrich.cpp](src/ticket_enrich.cpp)：
- HTTPS 用 `WiFiClientSecure::setInsecure()` 跳过证书校验，节省 ESP32 上 mbedTLS 握手资源
- JSON 解析复用 `ArduinoJson@^7.0.4`（已在 `platformio.ini` 中声明）
- 任意一步失败均不阻塞，字段保持空串，由渲染层兜底

性能参考：检票口 1 次 HTTPS ~2-4 秒；到达时间 2 次 HTTPS ~4-8 秒。

## 字段说明

`MailTicket` 当前包含：
- `date` （出行日期，如"2026-07-06"）
- `trainNo` （列车车次，如"GG7178"）
- `fromStation` （出发站，如"北京东"）
- `toStation` （到达站，如"北京西"）
- `departTime` （开车时间，如"08:13"）
- `arriveTime` （到达时间，如"10:30"）
- `carriage` （车厢号，如"5"）
- `seat` (座位号, 如"5F")
- `seatType` （席别，从全文正则提取，如"二等座"）
- `passenger`（乘车人，从全文正则提取）
- `orderNo`（EE 开头订单号，从全文正则提取）
- `checkPosition`（检票口，从全文正则提取"检票口XX"）

## 低功耗估算

- ESP32-D0WDQ6（V3 版本） deep sleep：~5 mA（含 RTC）
- 唤醒后平均功耗：~80 mA × 30s ≈ 2400 mA·s ≈ 0.67 mAh
- 每日总耗电：0.67 mAh / day
- CR2477 (1000 mAh) 理论续航：~ 4 年（理论值，实际受自放电与温度影响）

## 故障排查

1. **烧录失败**：检查 USB 驱动（CP210x/CH343），按住 BOOT 再上电。
2. **WiFi 连不上**：串口监视器会打印超时，确认 SSID/密码正确。
3. **IMAP `LOGIN failed`**：检查 `IMAP_USER` / `IMAP_PASS` 是否正确；126 邮箱需在网页版「设置 → POP3/SMTP/IMAP」开启 IMAP 服务。
4. **`no-future` 但邮箱里有票**：可能是邮件正文字段与解析器不匹配，可在串口查看 `[IMAP] UID xxxx 解析失败` 日志；或确认 `parsedCount` 是否符合预期。
5. **墨水屏不刷新**：检查接线；按 RST 重启一次。
