# 代码审查报告 2026-08-03

## 审查范围
- ESP32 端：`esp32/src/` 全部 C/C++ 源码（main.cpp, config_manager.cpp, wifi_mgr.cpp, rtc_sleep.cpp, epaper_render.cpp, ble_cfg.cpp, utils.cpp, config_defaults.cpp 及对应头文件）
- Server 端：`server/src/` 全部 Node.js 源码（index.js, config.js, utils.js, imap-fetcher.js, parser.js, ticket-finder.js, render-bitmap.js, arrive-time-fetcher.js, check-position-fetcher.js）

## 审查方法
1. 通读全部源码，推断作者意图
2. 识别 10 个问题，经 2 个子代理并行交叉验证（2/2 高置信度）
3. 全部修复

## 问题与修复清单

### 严重 (2)

| # | 文件 | 问题 | 修复方式 |
|---|------|------|---------|
| 1 | `esp32/src/epaper_render.cpp:37-41` | `offsetY` 无条件定义为 18 后又在 `#else` 分支重定义为 0，导致 SCREEN_212 未定义时重定义编译错误 | 将 `offsetY=18` 移入 `#if defined(SCREEN_212)` 分支 |
| 10 | `esp32/src/config_manager.cpp:60-74` | `nvsReadEncrypted` 解密失败时把密文当明文 fallback，AES_KEY 变更后旧密文会被误用为 IMAP 密码 | 解密失败时保持 dst 为空，让用户重新配置 |

### 重要 (6)

| # | 文件 | 问题 | 修复方式 |
|---|------|------|---------|
| 2 | `server/src/ticket-finder.js:103-114` | `new Date("HH:MM")` 得到 Invalid Date，行程耗时计算为 NaN | 新增 `parseTimeStr` / `parseTimeToMinutes` 辅助函数，正确解析 "HH:MM" 和 "HH:MM+N" 格式 |
| 3 | `esp32/src/main.cpp:27-34, 80-90` | `onErrorAndSleep` 注释说 300s/600s，代码实际是 30s/90s/270s；计数持久化位置注释错误（说 NVS 实为 RTC 内存） | 修正文件头和函数头注释与代码一致 |
| 4 | `esp32/src/config_defaults.cpp:70-78` | `configIsComplete` 要求 WiFi 密码非空，开放网络无法通过检查 | **已撤销** — 用户确认 pass 仍需校验，保持原代码不变 |
| 7 | `server/src/parser.js:186` | `cleanTicketBody` 正则 `[\n(\t\t\t)]` 会意外删除括号字符 | 改为 `/[\n\r\t]/g` |
| 8 | `server/src/parser.js:171` | `parseTicketFromText` 车次正则 `[A-Z]` 与 `parseOneSegment` 的 `[GCDZTKSLP1-9]` 不一致，数字开头车次漏匹配 | 统一为 `[GCDZTKSLP1-9]\d{1,4}` |
| 9 | `esp32/src/main.cpp:331-337` | `fetchBitmapFromServer` 注释说 FETCH_FAILED 时灾备回退，代码实际检查 FETCH_SERVER_ERROR | 修正注释为 FETCH_SERVER_ERROR |

### 次要 (2)

| # | 文件 | 问题 | 修复方式 |
|---|------|------|---------|
| 5 | `esp32/src/wifi_mgr.cpp:46-66` | `matched` 数组（含 rssi 字段）定义但未使用，注释承诺"按信号强度排序"但未实现 | 删除死代码，修正注释 |
| 6 | `esp32/src/rtc_sleep.cpp:12` | `BLINK_INTERVAL=15` 注释说"闪烁间隔 5s" | 修正注释为"闪烁间隔 15s" |

## 项目架构要点

### ESP32 端 (esp32/src/)
- **main.cpp**: 主入口，setup 单次执行流程（加载配置 → BLE 配网窗口 → WiFi → 推密钥 → 加密 IMAP → POST /ticket → 解密刷屏 → deep sleep）
- **config.h / config_defaults.cpp**: 全局配置定义 + NVS 持久化 + 完整性检查
- **config_manager.cpp**: NVS 读写，IMAP_PASS 用 AES 加密落盘，挂件图 blob 存储
- **wifi_mgr.cpp**: WiFi 扫描匹配 + 连接
- **rtc_sleep.cpp**: RTC 时间 + deep sleep 定时唤醒 + LED 闪烁倒计时
- **epaper_render.cpp**: GxEPD2 驱动，1bit 位图刷屏 + ASCII 文本显示
- **ble_cfg.cpp**: NimBLE BLE 配网，JSON 协议（get/set/reset/img_set/img_clear/ticket_refresh）
- **utils.cpp**: AES-128-CBC 加解密（mbedtls），与 server/src/utils.js 配对

### Server 端 (server/src/)
- **index.js**: Fastify HTTP 服务，POST /key（注册密钥）+ POST /ticket（抓票+渲染+签名去重+加密返回）
- **config.js**: datas.json 持久化（AES 密钥 + bitmap 签名 + IMAP 游标），写入串行化
- **utils.js**: AES-128-CBC 加解密，与 esp32/src/utils.cpp 配对
- **imap-fetcher.js**: IMAP UID 增量抓取 + 车票缓存
- **parser.js**: 12306 邮件正文解析（购票/退票/改签/候补）
- **ticket-finder.js**: 车票筛选（退票过滤 + 改签覆盖 + 取最近待出行）+ 检票口/到达时间补全
- **render-bitmap.js**: @napi-rs/canvas 渲染 1bit 位图（250x122）
- **arrive-time-fetcher.js**: 12306 到达时间补全（search → queryByTrainNo 两步）
- **check-position-fetcher.js**: 12306 检票口补全

### 关键协议
- **AES-128-CBC**: base64(IV(16) + 密文)，PKCS#7 填充，密钥为 32 字符 hex
- **POST /key**: { deviceId, key } → 写 datas.json aesKeys[deviceId]
- **POST /ticket**: Header X-Device-Id + { enstr } → 查密钥 → 解密 → IMAP 抓取 → 渲染 → 签名比对 → 加密 bitmap 返回
- **签名去重**: SHA-256 前 16 字符 hex，相同则 needUpdate=false 不下发 bitmap
- **BLE 配网**: NimBLE WRITE_NR + NOTIFY，JSON 按 \n 分帧
- **失败退避**: 30s → 90s → 270s → 屏显告警 + 正常唤醒周期
- **401 快重试**: 30s，重置 kp 标志，不计入失败计数

### 构建环境 (platformio.ini)
| 环境 | 芯片 | 关键宏 | 屏幕 |
|------|------|--------|------|
| esp32dev | ESP32 | RWB_SCREEN | 250x122 三色 |
| esp32s3 | ESP32-S3 | IS_ESP32S3 | 250x122 |
| seeed_xiao_esp32s3 | XIAO S3 | IS_xIAO_ESP32S3, SCREEN_212 | 212x104 |
| seeed_xiao_esp32s3_3C | XIAO S3 | IS_xIAO_ESP32S3, RWB_SCREEN | 250x122 三色 |
