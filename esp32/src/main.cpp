/**
 * 主入口 —— 服务端位图版（含 BLE 配网 + 动态 AES 密钥 + 签名去重）
 *
 * 工作流（setup 单次执行）：
 *   1. 加载 NVS 配置
 *   2. 检查必要字段：
 *        - 缺失 → 屏显 "CONFIG REQUIRED" + 直接进配网（5 分钟整体超时）
 *        - 齐备 → 继续
 *   3. 启动 15 秒 BLE 配网等待窗口：
 *        - 无人连接 → 继续抓票流程
 *        - 有人连接 → 进入 5 分钟会话超时
 *        - 配网完成 → 立即 restart；超时/取消 → deep sleep
 *   4. 连接 WiFi（失败 → 退避重试，见下文"失败退避策略"）
 *   5. 推送共享密钥到服务端（POST /key）——仅在 NVS kp 标志为 false 时执行
 *      （kp 在 configManagerSaveAll 时被重置：配网后首次启动 / 配置改动后再推一次）
 *      主→灾备回退：主服务端失败时尝试 TICKET_URL_1
 *   6. 用 NVS 密钥加密 IMAP 配置 → enstr
 *   7. POST {enstr} 到票务服务端（带 X-Device-Id header）
 *      主→灾备回退：主失败 / 401 时尝试 TICKET_URL_1
 *   8. 解析响应（needUpdate + hasTicket 两布尔字段）：
 *      - needUpdate=true + hasTicket=true  → 解密 bitmap 并刷屏
 *      - needUpdate=true + hasTicket=false → 走挂件图 fallback
 *      - needUpdate=false + hasTicket=true → 不刷屏，保留票面
 *      - needUpdate=false + hasTicket=false → 不刷屏，保留 fallback 画面
 *   9. 关闭 WiFi → deep sleep 到设定唤醒时间
 *
 * 失败退避策略（避免服务端长期不可用时空唤醒耗光电池）：
 *   - 失败 1 次 → 30s 后重试
 *   - 失败 2 次 → 90s 后重试
 *   - 失败 3 次 → 270s 后重试
 *   - 失败 ≥4 次 → 屏显 "Error" + 走正常唤醒周期
 *   - 401 设备未注册 → 30s 快重试（不计入计数；目的是尽快重新 POST /key）
 *   - 抓票成功 / BLE 改配 / 恢复出厂 → 失败计数清零
 *   计数持久化在 RTC 内存（RTC_DATA_ATTR），跨 deep sleep 保留
 *
 * 位图签名去重协议（v5）：
 *   - 服务端对 bitmap 计算 SHA-256 签名并持久化，与上次比对
 *   - 仅签名变化时下发 bitmap（needUpdate=true），否则只回两个布尔字段
 *   - 硬件端据此避免无效刷屏，延长墨水屏寿命
 */
#include <Arduino.h>
#include <SPI.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "config.h"
#include "config_manager.h"
#include "wifi_mgr.h"
#include "rtc_sleep.h"
#include "epaper_render.h"
#include "ble_cfg.h"
#include "utils.h"

// RTC 内存：掉电不丢失，存业务倒计时和LED状态
RTC_DATA_ATTR int errorTryCount = 0;

/**
 * 构造 IMAP 配置明文："user|pass|host|port|useSecure"
 * 顺序与 server/src/index.js#POST /ticket 的解密逻辑一致
 */
static String buildImapPlain() {
  String plain;
  plain.reserve(128);
  plain += String(IMAP_USER);
  plain += '|';
  plain += String(IMAP_PASS);
  plain += '|';
  plain += String(IMAP_HOST);
  plain += '|';
  plain += String(IMAP_PORT);
  plain += '|';
  /* IMAP_USE_SECURE 是编译期常量（v4 强制 SSL/TLS） */
  plain += (IMAP_USE_SECURE ? "true" : "false");
  return plain;
}

/**
 * 抓票失败时休眠一段时间后重试（保留上次的位图）
 * 退避策略：基于 RTC 内存中的"连续失败计数 errorTryCount"做退避，避免网络/服务端
 *   长期不可用时一直短周期唤醒耗光电池：
 *     - 失败 1 次 → 30s 后重试
 *     - 失败 2 次 → 90s 后重试
 *     - 失败 3 次 → 270s 后重试
 *     - 失败 ≥4 次 → 认为已"彻底不可用"，屏显告警后走正常唤醒周期
 *   成功抓票 / 用户改配 / 恢复出厂 都会清零 errorTryCount
 * @param reason 错误原因（仅日志）
 */
static void onErrorAndSleep(const String &reason) {
  
  errorTryCount++;

  Serial.printf("[BOOT] 错误: %s, 累计失败 %u 次\n", reason.c_str(), (unsigned)errorTryCount);

  if (errorTryCount > 3) {
    // 清空本次失败计数
    errorTryCount = 0;
    /* 连续 3 次失败：放弃短周期快重试，避免电池被空唤醒耗光。
       屏显告警 + 切到正常唤醒周期，等用户察觉后主动重配/排查。 */
    Serial.println("[BOOT] 连续失败达到上限，屏显告警并按正常唤醒周期等待");
    String errorMsg = String("Error：\n") + reason;
    epaperDrawText(errorMsg.c_str(), 4, 36);
    epaperHibernate();
    deepSleepUntil(WAKE_HOUR, WAKE_MINUTE);
  }

  /* cnt = 1 → 30s;  cnt = 2 → 90s;  cnt = 3 → 270s */
  uint32_t backoffSec = (errorTryCount == 1) ? 30 :
                        (errorTryCount == 2) ? 90 : 270;
  Serial.printf("[BOOT] %u 秒后重试\n", (unsigned)backoffSec);
  
  deepSleepFor(backoffSec);
}

/**
 * 401 设备未注册的特殊重试：固定 30s 快重试，不计入"网络失败"计数
 * 原因：401 = 服务端 datas.json 丢了我们的密钥，需要尽快重新 POST /key。
 *   如果走标准退避，第一次重试就得等 5min，错过恢复窗口；
 *   连续 401 时仍应快速重试（kp 每次都被重置 → 下次启动必重新推密钥）。
 *   真正持久的网络故障会在后续 POST /key 阶段被 catch 走标准退避路径。
 */
static void onUnregisteredAndRetry() {
  Serial.println("[BOOT] 设备未注册，重置 kp 标志，30 秒后重启重推密钥");
  configManagerResetKeyPushed();
  epaperHibernate();
  deepSleepFor(30);
}

/**
 * 把 TICKET_URL 末尾的 path 替换为 /key
 * 例：http://host:8080/ticket -> http://host:8080/key
 *     https://api.example.com/v1/ticket -> https://api.example.com/v1/key
 *     http://host:8080 -> http://host:8080/key
 */
static String deriveKeyUrl(const char *ticketUrl) {
  String url = String(ticketUrl);
  int proto = url.indexOf("://");
  if (proto < 0) return url + "/key";
  int lastSlash = url.lastIndexOf('/');
  if (lastSlash > proto + 2) {
    return url.substring(0, lastSlash) + "/key";
  }
  return url + "/key";
}

/**
 * 把 AES_KEY 推送到指定 URL 的 /key 端点
 * @param baseUrl 票务服务端 URL（TICKET_URL 或 TICKET_URL_1）
 * @return true 推送成功（HTTP 200），false 网络/HTTP 错误
 */
static bool tryPushKeyTo(const char *baseUrl) {
  String url = deriveKeyUrl(baseUrl);
  Serial.printf("[KEY] 推送共享密钥到 %s (deviceId=%s)\n",
                url.c_str(), makeDeviceId().c_str());

  HTTPClient http;
  if (!http.begin(url)) {
    Serial.println("[KEY] http.begin 失败");
    return false;
  }
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");

  String body = String("{\"deviceId\":\"") + makeDeviceId()
              + "\",\"key\":\"" + String(AES_KEY) + "\"}";
  int code = http.POST(body);
  http.end();

  if (code == 200) {
    Serial.println("[KEY] 推送成功");
    return true;
  }
  Serial.printf("[KEY] 推送失败: HTTP %d\n", code);
  return false;
}

/**
 * 把 NVS 中的 AES_KEY 推送到服务端（POST /key）
 * 主→灾备回退：主失败时尝试 TICKET_URL_1；任一成功即视为推送成功。
 * 服务端会按 deviceId 写入 config.aesKeys[deviceId]，双方持久化。
 * @return true 至少有一台服务端确认（已注册），false 全部失败
 */
static bool pushKeyToServer() {
  if (AES_KEY[0] == '\0') {
    Serial.println("[KEY] 跳过推送：AES_KEY 未配");
    return false;
  }
  if (tryPushKeyTo(TICKET_URL)) return true;
  if (TICKET_URL_1[0] != '\0') {
    Serial.println("[KEY] 主服务端推送失败，尝试灾备");
    if (tryPushKeyTo(TICKET_URL_1)) return true;
  }
  return false;
}

/**
 * 抓票结果分类（用于 main.cpp 决定显示 / 不显示 / fallback / 重试）
 *  - FETCH_OK                   needUpdate=true + hasTicket=true：服务端发了有效 bitmap，已刷屏
 *  - FETCH_UPDATE_NO_TICKET     needUpdate=true + hasTicket=false：bitmap 有变化但当前无票，走挂件图 fallback
 *  - FETCH_NO_UPDATE_HAS_TICKET needUpdate=false + hasTicket=true：bitmap 未变且有票，保留画面不刷屏
 *  - FETCH_NO_UPDATE_NO_TICKET  needUpdate=false + hasTicket=false：bitmap 未变且无票，保留上次 fallback 画面
 *  - FETCH_UNREGISTERED         服务端 401（设备未注册，datas.json 误删等）→ 需重置 kp 重新推送密钥
 *  - FETCH_FAILED               网络/HTTP/JSON 解码/AES 解密/刷屏 任一步失败
 *  - FETCH_SERVER_ERROR         服务端返回其他错误码，如 500、400 等
 */
enum FetchResult {
  FETCH_OK = 0,
  FETCH_UPDATE_NO_TICKET = 1,
  FETCH_NO_UPDATE_HAS_TICKET = 2,
  FETCH_NO_UPDATE_NO_TICKET = 3,
  FETCH_UNREGISTERED = 4,
  FETCH_FAILED = 5,
  FETCH_SERVER_ERROR = 6,
};

/**
 * 强刷标志：BLE 配网中用户点了"刷新车票"→ 退出配网后由 main.cpp 走抓票流程，
 * 此标志控制 POST /ticket 时是否带 X-Force-Render: 1 header（跳过服务端所有缓存）。
 */
static bool g_forceRefresh = false;

/**
 * 向指定票务服务端发 POST 拉取位图状态
 * @param baseUrl 服务端 URL（TICKET_URL 或 TICKET_URL_1）
 * @param enstr 加密后的 IMAP 配置
 * @return 五态 enum（见 FetchResult 定义）
 *
 * 协议：服务端响应固定包含 { ok:true, needUpdate, hasTicket, sign, bitmap?(仅 needUpdate=true) }
 *  - needUpdate: bitmap 是否有变化（true=需刷新屏幕，false=保留画面）
 *  - hasTicket:  当前是否有有效车票（true=有票，false=无票）
 *  - 仅 needUpdate=true 时响应才带 bitmap 字段
 */
static FetchResult tryFetchBitmapFrom(const char *baseUrl, const String &enstr) {
  HTTPClient http;
  String url = String(baseUrl);
  Serial.printf("[HTTP] POST %s (enstr.len=%u)\n", url.c_str(), (unsigned)enstr.length());


  if (!http.begin(url)) {
    Serial.println("[HTTP] begin 失败");
    return FETCH_FAILED;
  }
  http.setTimeout(TICKET_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Id", makeDeviceId());
  http.addHeader("X-Screen-W", String(EPD_WIDTH));
  http.addHeader("X-Screen-H", String(EPD_HEIGHT));
  if (g_forceRefresh) {
    http.addHeader("X-Force-Render", "1");
  }
  String body = String("{\"enstr\":\"") + enstr + "\"}";

  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("[HTTP] 状态码: %d (url=%s)\n", code, url.c_str());
    if (code > 0) {
      String resp = http.getString();
      Serial.printf("[HTTP] 响应: %s\n", resp.c_str());
    }
    http.end();
    /* 401 = 设备未注册（服务端 datas.json 误删 / 密钥丢失），
       需重置 kp 标志，下次启动重新 POST /key 推送密钥，避免死锁 */
    if (code == 401) {
      Serial.println("[HTTP] 401 设备未注册");
      return FETCH_UNREGISTERED;
    }
    return FETCH_SERVER_ERROR;
  }

  String resp = http.getString();
  http.end();

  // 及时释放 WiFi 连接，避免耗电
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  delay(150);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, resp);
  if (err) {
    Serial.printf("[JSON] 解析失败: %s\n", err.c_str());
    return FETCH_FAILED;
  }

  time_t serverTs = doc["ts"].as<time_t>();
  Serial.printf("[HTTP] 服务器时间戳: %u\n", (unsigned)serverTs);
  setRTCTime(serverTs);
  printLocalTime();

  bool needUpdate = doc["needUpdate"] | false;
  bool hasTicket = doc["hasTicket"] | false;
  Serial.printf("[HTTP] needUpdate=%d hasTicket=%d\n", needUpdate ? 1 : 0, hasTicket ? 1 : 0);

  if (needUpdate) {
    /* 先判断 hasTicket：无票时服务端不下发 bitmap，硬件端直接走 fallback，
       避免解密/刷屏 DEFAULT_EMPTY_TICKET 假票后再被覆盖（省一次全刷 + AES 解密） */
    if (!hasTicket) {
      Serial.println("[HTTP] needUpdate=true + hasTicket=false：走挂件图 fallback（无 bitmap）");
      return FETCH_UPDATE_NO_TICKET;
    }
    const char *b64 = doc["bitmap"].as<const char *>();
    if (!b64) {
      Serial.println("[HTTP] needUpdate=true + hasTicket=true 但响应中无 bitmap 字段");
      return FETCH_FAILED;
    }
    String decryptedBitmap = aesDecryptBase64(b64, AES_KEY);
    Serial.printf("[HTTP] 解密位图数据长度: %u\n", (unsigned)decryptedBitmap.length());
    if (decryptedBitmap.length() == 0) {
      Serial.println("[HTTP] 解密位图失败（密钥不匹配？）");
      return FETCH_FAILED;
    }
    if (!epaperDrawBitmap((const uint8_t *)decryptedBitmap.c_str(), decryptedBitmap.length())) {
      Serial.println("[HTTP] 绘制位图失败");
      return FETCH_FAILED;
    }
    Serial.println("[HTTP] needUpdate=true + hasTicket=true：已刷票面");
    return FETCH_OK;
  }

  /* needUpdate=false：不刷屏，按 hasTicket 返回对应状态 */
  if (hasTicket) {
    Serial.println("[HTTP] needUpdate=false + hasTicket=true：保留票面，不刷屏");
    return FETCH_NO_UPDATE_HAS_TICKET;
  }
  Serial.println("[HTTP] needUpdate=false + hasTicket=false：保留上次 fallback 画面");
  return FETCH_NO_UPDATE_NO_TICKET;
}

/**
 * 拉取最新位图状态：主→灾备回退
 * 主返回 FETCH_SERVER_ERROR 时尝试 TICKET_URL_1；
 * 主成功（含 4 个非失败态）直接返回；灾备未配置时只走主。
 * @param enstr 加密后的 IMAP 配置
 * @return 五态 enum（见 FetchResult 定义）
 */
static FetchResult fetchBitmapFromServer(const String &enstr) {
  FetchResult r = tryFetchBitmapFrom(TICKET_URL, enstr);
  if (r != FETCH_SERVER_ERROR) return r;
  /* 主失败：网络错；灾备可能正常 */
  if (TICKET_URL_1[0] != '\0') {
    Serial.println("[HTTP] 主服务端不可用，尝试灾备");
    return tryFetchBitmapFrom(TICKET_URL_1, enstr);
  }
  return r;
}

/**
 * 无票 fallback 显示：优先显示挂件图（用户在 BLE 配网页面上传的 1bit 图），
 * 无图则显示 "NO TICKET" ASCII 文本。
 * 调用后墨水屏已显示内容，调用方只需 epaperHibernate() + 进 deep sleep。
 */
static void showNoTicketFallback() {
  if (configManagerHasIdleImage()) {
    Serial.println("[FALLBACK] NVS 有挂件图，优先显示");
    /* 用 static 避开 stack 上 3904B 大数组（loopTask 栈 16KB 虽够但留余量） */
    static uint8_t buf[EPD_BITMAP_SIZE];
    if (configManagerLoadIdleImage(buf, EPD_BITMAP_SIZE)) {
      if (epaperDrawBitmap(buf, EPD_BITMAP_SIZE)) {
        return;
      }
      Serial.println("[FALLBACK] 挂件图刷屏失败，回退文本");
    } else {
      Serial.println("[FALLBACK] 挂件图读取失败，回退文本");
    }
  } else {
    Serial.println("[FALLBACK] 无挂件图，显示 NO TICKET 文本");
  }
  /* 回退：显示 ASCII 文本
     这里 screenReady 已经 true（上面 controllerOnly 调过），所以幂等直接进 drawText */
  epaperDrawText("NO TICKET", 70, 70);
}

/**
 * 上电时若缺配置：屏显提示 + 直接进入配网（不等用户按 BOOT）
 * 配网结束根据结果决定 esp_restart（完成 → 立即抓票）
 * 或 deepSleepUntil（超时 / 取消 → 等用户下次机会）
 */
static void enterConfigModeAndSleep() {
  Serial.println("[BOOT] 配置缺失，直接进入配网模式");
  // 合并 4 次全刷（init + 3 次 drawText）为 1 次：节省 ~75s
  // 三行文本用 \n 换行（依赖 epaper_render.cpp 中 yAdvance 实现的行间距）
  epaperDrawText("All configs required !!!\nBLE: TicketBadge-Cfg\nConnecting...", 4, 36);
  epaperHibernate();

  CfgResult r = bleCfgEnter();
  if (r == CFG_RESULT_COMPLETED) {
    /* 用户刚把缺的那几项填好 → 立即重启让 setup 走抓票流程，
       避免按 24h 睡眠让用户等一天才看到第一张票 */
    Serial.println("[BOOT] 配网完成，立即重启进入抓票流程");
    WiFi.mode(WIFI_OFF);
    delay(1000);  // 等 WiFi 断开后重启（曾用 3s，优化为 1s 缩短总耗时）
    esp_restart();
    return;
  }
  if (r == CFG_RESULT_REFRESH) {
    /* 用户点了"刷新车票"→ 不重启，直接走 setup() 的抓票流程（带强刷标识） */
    g_forceRefresh = true;
    Serial.println("[BOOT] 配网中用户请求强刷车票，直接进入抓票流程");
    return;  /* fall through to setup() 的 WiFi → fetch 流程 */
  }
  // 配网超时 / 取消：屏显提示 + 等待用户下次机会
  Serial.println("[BOOT] 配网超时 / 取消，等待用户下次机会");
  epaperDrawText("Configs not complete.\n Please restart.", 0, 8);
  /* TIMEOUT / CANCELED：按设定的唤醒时间 deep sleep。
     - 缺配置场景下 WAKE_HOUR/WAKE_MINUTE=255，secondsUntilNextWake 兜底 24h
     - 若 RTC 也没时间（首次启动从未连过 WiFi），同样 24h 兜底
     这样避免 30 秒短周期循环刷屏耗光电池（用户下次启动时还会再开 30 秒 BLE 等待窗口） */
  deepSleepUntil(WAKE_HOUR, WAKE_MINUTE);
}

/**
 * 判断是否为冷启动（真正上电 / 手动 RST），而非 deep sleep 唤醒
 *
 * 用途：BLE 配网等待窗口只在冷启动时开，避免每次 24h 唤醒都白白耗 15s BLE radio。
 * deep sleep timer 唤醒返回 ESP_SLEEP_WAKEUP_TIMER；其他唤醒源未启用，
 * 故除 UNDEFINED 外的都视为"唤醒"，跳过 BLE 窗口直接抓票。
 *
 * 用户改 WiFi 密码后的应对：手动按 RST 键触发冷启动 → 自动进 BLE 配网窗口。
 * @return true 表示冷启动
 */
static bool isColdBoot() {
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED;
}

/**
 * 启动时 15 秒 BLE 配网等待窗口（仅配置完整 + 冷启动时调用）
 *   - 15 秒内无人连接 (NO_CONNECT) → 落到下方抓票流程
 *   - 15 秒内有人连接 → 进入 5 分钟会话超时
 *   - 配网完成 (COMPLETED) → 立即重启抓票
 *   - 会话超时 / 取消 (TIMEOUT/CANCELED) → deep sleep 等下次唤醒
 *
 * 不刷屏：保留上次画面，用户大概率没在看屏幕；
 * 若用户想配网，会主动用手机连 BLE
 *
 * @return true 表示已处理（应直接 return 退出 setup），false 表示继续抓票流程
 */
static bool tryBleConfigOnBoot() {
  Serial.println("[BOOT] 启动 15 秒 BLE 配网等待窗口");
  CfgResult r = bleCfgEnter(15000);
  if (r == CFG_RESULT_COMPLETED) {
    Serial.println("[BOOT] 配网完成，立即重启进入抓票流程");
    esp_restart();
  }
  if (r == CFG_RESULT_REFRESH) {
    /* 用户点了"刷新车票"→ 退出配网，直接走抓票流程（带强刷标识） */
    g_forceRefresh = true;
    Serial.println("[BOOT] 用户请求强刷车票，退出配网进入抓票流程");
    return false;
  }
  Serial.println("[BOOT] 15秒内无 BLE 连接及配置更新，继续抓票流程");
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // 闪烁 LED 并倒计时
  blinkLedAndSleepCountdown(WAKE_HOUR, WAKE_MINUTE);

  Serial.println();
  Serial.println("===== 迷你墨水屏高铁票挂件 (v5: 签名去重 + 动态密钥) =====");
  Serial.printf("[BOOT] 设备ID: %s\n", makeDeviceId().c_str());


  /* 1) 加载 NVS 配置 */
  configManagerBegin();

  /* 2) 缺配置：屏显 + 直接进配网（5 分钟整体超时） */
  if (!configIsComplete()) {
    enterConfigModeAndSleep();
  }

  /* 3) BLE 配网等待窗口（仅冷启动时开，省 24h 周期下的 15s BLE radio 耗电）
        - 冷启动：开 15s BLE 窗口给用户改配的机会
        - 唤醒：直接跳过，进入抓票流程
        - 强刷（g_forceRefresh=true）：跳过 BLE 窗口直接抓票
        - 用户改 WiFi 密码后：按 RST 键触发冷启动 → 自动进 BLE 窗口
        - brownout 复位（3.7V 锂电池无电容缓冲，WiFi TX 峰值拉低电压触发）：
            直接 deep sleep 1 小时等电池电压恢复，避免 brownout 死循环耗光电池。
            3.7V 锂电池无电容缓冲时，WiFi TX 240mA 峰值会让 LDO 输出跌到 brownout 阈值
            以下触发复位，复位后又立刻抓票 → 又 brownout → 又复位 → 电池耗光。
            注：电池电压恢复到 3.8V 以上才可能稳定抓票。
        - wdt / panic 复位：软件 bug，短重试看是否能恢复 */
  if (g_forceRefresh) {
    /* 从 enterConfigModeAndSleep() REFRESH 返回：跳过 BLE 窗口直接抓票 */
    Serial.println("[BOOT] 强刷模式，跳过 BLE 窗口直接抓票");
  } else if (isColdBoot()) {
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_BROWNOUT) {
      /* brownout：电池撑不住 WiFi 峰值，长休眠等电压恢复
         1 小时后唤醒再试，若仍 brownout 则继续 1 小时，等用户充电 / 换大电容 */
      Serial.println("[BOOT] brownout 复位，电池电压不足，1 小时后重试");
      deepSleepFor(3600);
      return;  /* deepSleepFor 不会返回，防御性 */
    }
    bool skipBle = (rr == ESP_RST_TASK_WDT)   /* 任务看门狗 */
                || (rr == ESP_RST_INT_WDT)    /* 中断看门狗 */
                || (rr == ESP_RST_PANIC);     /* 异常 panic */
    if (skipBle) {
      Serial.printf("[BOOT] 复位原因=%d（wdt/panic），跳过 BLE 窗口省电\n", (int)rr);
    } else {
      if (tryBleConfigOnBoot()) {
        return;  // 已 deep sleep / restart，不会到这
      }
    }
  } else {
    Serial.println("[BOOT] deep sleep 唤醒，跳过 BLE 配网窗口直接抓票");
  }

  /* 5) 连接 WiFi */
  if (!connectWifi()) {
    Serial.println("[BOOT] WiFi 失败");
    onErrorAndSleep("WiFi Failed");
  }

  /* 6) 推送共享密钥到服务端
   *    触发条件：NVS 中 kp 标志 == false（首次配对 / 后续被 set 重置）。
   *    已推送过（kp=true）则直接跳过，避免每次唤醒都重复 POST /key。
   *    推送失败 → 服务端没此设备密钥 → /ticket 必然解密失败，
   *    所以**中止**抓票，进入短周期 deep sleep 等待下次唤醒重试 */
  if (!configManagerIsKeyPushed()) {
    Serial.println("[BOOT] kp=false，需要推送共享密钥到服务端");
    if (!pushKeyToServer()) {
      Serial.println("[BOOT] 密钥推送失败，中止抓票流程");
      onErrorAndSleep("Key Push Failed");
    }
    configManagerMarkKeyPushed();
    Serial.println("[BOOT] 密钥已标记为已推送");
  } else {
    Serial.println("[BOOT] kp=true，跳过密钥推送");
  }

  /* 7) 加密 IMAP 配置 */
  String plain = buildImapPlain();
  Serial.printf("[AESS] 加密明文长度: %u\n", (unsigned)plain.length());
  String enstr = aesEncryptBase64(plain, AES_KEY);
  if (enstr.length() == 0) {
    onErrorAndSleep("AES Encrypt Failed");
  }
  Serial.printf("[AESS] enstr 长度: %u\n", (unsigned)enstr.length());

  /* 8) 拉取并按 needUpdate/hasTicket 四态分支 */
  FetchResult fr = fetchBitmapFromServer(enstr);
  /* 成功四态：清零连续失败计数 → 下次失败从 300s 退避起算，
     避免"上次失败很多次 + 这次成功"的下一次失败立即进告警状态 */
  bool ok = (fr == FETCH_OK || fr == FETCH_UPDATE_NO_TICKET ||
             fr == FETCH_NO_UPDATE_HAS_TICKET || fr == FETCH_NO_UPDATE_NO_TICKET);
  if (ok) {
    errorTryCount = 0;
  }
  switch (fr) {
    case FETCH_OK:
      /* needUpdate=true + hasTicket=true：已刷服务端 bitmap，屏幕已更新 */
      Serial.println("[BOOT] 已刷票面");
      break;
    case FETCH_UPDATE_NO_TICKET:
      /* needUpdate=true + hasTicket=false：bitmap 有变化但无票，走挂件图 fallback */
      showNoTicketFallback();
      break;
    case FETCH_NO_UPDATE_HAS_TICKET:
      /* needUpdate=false + hasTicket=true：bitmap 未变且有票，完全不动屏幕，保留上次票面 */
      Serial.println("[BOOT] bitmap 未变，保留票面（未调任何 display API）");
      break;
    case FETCH_NO_UPDATE_NO_TICKET:
      /* needUpdate=false + hasTicket=false：bitmap 未变且无票，保留上次 fallback 画面 */
      Serial.println("[BOOT] 无票且 bitmap 未变，保留 fallback 画面");
      break;
    case FETCH_UNREGISTERED:
      /* 服务端 401：datas.json 误删 / 密钥丢失 → 重置 kp，快速重试。
         不计入"网络失败"计数（详见 onUnregisteredAndRetry 注释） */
      onUnregisteredAndRetry();
      break;
    case FETCH_SERVER_ERROR:
    case FETCH_FAILED:
      /* 网络 / HTTP / 解析 / 解密 / 刷屏 错误 → 走 onErrorAndSleep 退避策略 */
      onErrorAndSleep("HTTP /ticket Failed");
  }

  /* 9) 休眠墨水屏
     注意 needUpdate=false 路径下 screenReady=false，epaperHibernate() 内部直接 return，
     不触发 SPI 通信，与"保留画面"语义一致 */
  epaperHibernate();

  /* 10) 进入 deep sleep（WiFi/Serial/GPIO/电源域由 deepSleepUntil 内部统一关闭） */
  deepSleepUntil(WAKE_HOUR, WAKE_MINUTE);
}

/**
 * loop() 正常流程永不执行：setup() 末尾必走 deepSleepUntil / onErrorAndSleep /
 * onUnregisteredAndRetry 三选一进入 deep sleep。
 * 若意外落到此（逻辑漏洞），旧实现是 30s active delay 满负荷耗电；
 * 现改为立即进入 deep sleep 等下次唤醒，避免异常场景下的高功耗。
 */
void loop() {
  Serial.println("[LOOP] 意外进入 loop()，立即 deep sleep 等下次唤醒");
  Serial.flush();
  deepSleepUntil(WAKE_HOUR, WAKE_MINUTE);
}
