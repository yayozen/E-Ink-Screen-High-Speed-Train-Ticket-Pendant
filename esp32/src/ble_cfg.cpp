/**
 * BLE 配网实现
 *
 * NimBLE GATT 布局：
 *   Service      : 4a4b5c6d-1111-2222-3333-444455556666
 *     - WRITE  (WRITE)                   : 客户端命令（流式 20B 分包），累积到 \n 解析
 *     - NOTIFY (NOTIFY, max=512)         : 服务端响应推送
 *
 * 配网页面 HTML 由用户自行通过 docs/cfg.html 分发（手机/电脑浏览器打开后
 * 调 Web Bluetooth API 连接 ESP32；BLE 内不再内嵌 HTML）。
 *
 * 协议：
 *   WRITE  →  {"type":"get"}\n
 *           / {"type":"set",...}\n
 *           / {"type":"reset"}\n
 *           / {"type":"img_set","b64":"...","w":250,"h":122}\n   ← 新增：上传挂件图
 *           / {"type":"img_clear"}\n                            ← 新增：清除挂件图
 *           / {"type":"img_preview","b64":"...","w":..,"h":..}\n ← 新增：预览挂件图
 *           / {"type":"ticket_refresh"}\n                       ← 强刷车票第1步：请求确认
 *           / {"type":"ticket_refresh_confirm"}\n               ← 强刷车票第2步：确认→退出配网
 *   NOTIFY ←  {"type":"config", data:{...}}\n
 *           / {"type":"result", ok:bool, action:"set|reset|img_set|img_clear|ticket_refresh|ticket_refresh_confirm", ...}\n
 *           / {"type":"error",...}\n
 * 注：img_set / img_clear / img_preview 落盘/刷屏后不退出配网（g_bleQuit=false），
 *    配网页面应继续发 set 命令触发 g_bleQuit=true 才整体保存退出。
 *    ticket_refresh_confirm 会退出配网（g_bleQuit=true, result=REFRESH），
 *    main.cpp 据此走抓票流程（带 X-Force-Render:1 强刷）。
 */
#include "ble_cfg.h"
#include "config.h"
#include "config_manager.h"
#include "epaper_render.h"   /* EPD_BITMAP_SIZE */

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>  /* img_set 命令的 b64 解码 */
#include <esp_sleep.h>
#include <string.h>

/* ============== 配网内部状态 ============== */

/* 配网模式超时（5 分钟） */
static const uint32_t CFG_BLE_TIMEOUT_MS = 5UL * 60 * 1000;

/* WRITE characteristic：接收累积 buffer */
static String g_rxBuf;

/* 全局 characteristic 句柄（onWrite / notify 用） */
static NimBLECharacteristic *g_chrW = nullptr;
static NimBLECharacteristic *g_chrN = nullptr;

/* 配网模式控制标志 */
static volatile bool g_bleQuit = false;
static volatile bool g_bleConnected = false;
/* 配网退出原因（默认 TIMEOUT，set/reset 成功时改 COMPLETED，
   客户端主动断开时由 onDisconnect 升级为 CANCELED） */
static volatile CfgResult g_bleResult = CFG_RESULT_TIMEOUT;

/* ============== 内部：notify 推送工具 ============== */

/**
 * 把一行 JSON 通过 NOTIFY characteristic 推给浏览器
 * （自动加 \n 结束符；按当前协商 MTU 切片发送，避免单包超 MTU 被 NimBLE 静默丢弃）
 *
 * 设计依据：NimBLE-Arduino 1.4.x 的 notify() 不做自动分片，若 len > MTU-3
 * 会直接返回 -1 静默丢弃；configManagerDumpJson() 的输出含完整字段约 280B，
 * 在 Windows Chrome 上 MTU 协商可能落到 23-69，必然超长。
 * 浏览器侧按 \n 累积解析，故可在 \n 之前任意位置切分。
 */
static void notifyLine(const String &line) {
  if (!g_chrN) {
    Serial.println("[BLE] notify 失败：g_chrN 为 null");
    return;
  }
  String withNl = line + "\n";
  /* 当前协商后 MTU（ATT 层），减去 3 字节头得到本帧最大 payload */
  const uint16_t mtu = NimBLEDevice::getMTU();
  const uint16_t maxChunk = (mtu > 3) ? (uint16_t)(mtu - 3) : 20;
  Serial.printf("[BLE] notify total=%u mtu=%u chunk=%u\n",
                (unsigned)withNl.length(), (unsigned)mtu, (unsigned)maxChunk);
  const uint8_t *p = (const uint8_t *)withNl.c_str();
  uint16_t remaining = (uint16_t)withNl.length();
  /* 按协商后 MTU 切片发送（NimBLE-Arduino 1.4.x 的 notify() 返回 void，
     不做自动分片；若 len > MTU-3 内部会静默丢弃，浏览器永远收不到） */
  while (remaining > 0) {
    uint16_t chunk = (remaining > maxChunk) ? maxChunk : remaining;
    g_chrN->notify(p, chunk);
    Serial.printf("[BLE]   sent chunk=%u/%u\n", (unsigned)chunk, (unsigned)withNl.length());
    p += chunk;
    remaining -= chunk;
    if (remaining > 0) {
      /* 给 NimBLE stack 时间把上一帧发出去，避免过快堆积丢包 */
      delay(8);
    }
  }
}

/**
 * 发送 result 响应
 */
static void sendResultOk(const char *action, const String &saved) {
  JsonDocument doc;
  doc["type"] = "result";
  doc["ok"] = true;
  if (action) doc["action"] = action;
  if (saved.length() > 0) doc["saved"] = saved;
  String out;
  serializeJson(doc, out);
  notifyLine(out);
}

static void sendError(const char *msg) {
  JsonDocument doc;
  doc["type"] = "error";
  doc["message"] = msg;
  String out;
  serializeJson(doc, out);
  notifyLine(out);
}

static void sendCurrentConfig() {
  JsonDocument doc;
  doc["type"] = "config";
  JsonDocument data;
  configManagerDumpJson(data);
  /* 屏幕尺寸（编译时常量）下发给浏览器，用于 canvas 尺寸 + 抖动 + 预览 */
  data["screen"]["w"] = EPD_WIDTH;
  data["screen"]["h"] = EPD_HEIGHT;
  doc["data"] = data;
  String out;
  serializeJson(doc, out);
  notifyLine(out);
}

/* ============== 内部：处理客户端命令 ============== */

static void handleSetCommand(const JsonDocument &doc) {
  String saved = configManagerApplyJson(doc);
  sendResultOk("set", saved);
  Serial.printf("[BLE] 配置已写入: %s\n", saved.c_str());
  /* 给浏览器 2 秒时间断开，然后我们自己退出 */
  g_bleResult = CFG_RESULT_COMPLETED;
  g_bleQuit = true;
}

static void handleResetCommand() {
  configManagerResetToDefaults();
  sendResultOk("reset", "");
  Serial.println("[BLE] 已恢复出厂");
  g_bleResult = CFG_RESULT_COMPLETED;
  g_bleQuit = true;
}

/**
 * img_set 命令处理：解析浏览器端上传的 1bit base64 挂件图并写入 NVS
 *
 * 协议：{"type":"img_set","b64":"<5.2KB base64>","w":250,"h":122}
 * 时序估算：~260 帧 × 10ms ≈ 2.6s 接收 + 10ms base64 decode + 30ms NVS putBytes ≈ 3s
 *          远低于 BLE supervision timeout 20s，可同步处理
 * 不退出配网：图片独立落盘，用户点"保存配置并重启"按钮再统一发 set 触发 g_bleQuit
 *             这样传完图不必等"已连接 2 秒"断开重连
 */
static void handleImgSetCommand(const JsonDocument &doc) {
  const char *b64 = doc["b64"] | "";
  if (strlen(b64) == 0) {
    sendError("img_set: empty b64");
    return;
  }

  /* 校验客户端声明的尺寸（防御性：拒掉非 EPD_WIDTH x EPD_HEIGHT 的图） */
  int w = doc["w"] | 0;
  int h = doc["h"] | 0;
  if (w != EPD_WIDTH || h != EPD_HEIGHT) {
    Serial.printf("[BLE] img_set 尺寸非法: %dx%d (期望 %dx%d)\n", w, h, EPD_WIDTH, EPD_HEIGHT);
    sendError("img_set: bad size");
    return;
  }

  size_t b64Len = strlen(b64);
  /* mbedtls_base64_decode 要求 out 缓冲 ≥ 3*b64Len/4 + 4，预留余量 */
  const size_t outCap = (b64Len * 3) / 4 + 4;
  uint8_t *buf = (uint8_t *)malloc(outCap);
  if (!buf) {
    sendError("img_set: OOM");
    return;
  }
  size_t outLen = 0;
  int rc = mbedtls_base64_decode(buf, outCap, &outLen,
                                 (const uint8_t *)b64, b64Len);
  if (rc != 0 || outLen != EPD_BITMAP_SIZE) {
    Serial.printf("[BLE] img_set base64 解码失败 rc=%d outLen=%u (期望 %u)\n",
                  rc, (unsigned)outLen, (unsigned)EPD_BITMAP_SIZE);
    free(buf);
    sendError("img_set: bad b64");
    return;
  }

  bool ok = configManagerSaveIdleImage(buf, EPD_BITMAP_SIZE);
  free(buf);
  if (!ok) {
    sendError("img_set: NVS write failed");
    return;
  }
  sendResultOk("img_set", "idleImage");
  Serial.println("[BLE] 挂件图已落 NVS（不退出配网，等用户点保存配置并重启）");
}

/**
 * img_preview 命令处理：解析浏览器端上传的 1bit base64 挂件图并直接渲染到屏幕
 */
static void handleImgPreviewCommand(const JsonDocument &doc) {
  const char *b64 = doc["b64"] | "";
  if (strlen(b64) == 0) {
    sendError("img_preview: empty b64");
    return;
  }

  /* 校验客户端声明的尺寸（防御性：拒掉非 EPD_WIDTH x EPD_HEIGHT 的图） */
  int w = doc["w"] | 0;
  int h = doc["h"] | 0;
  if (w != EPD_WIDTH || h != EPD_HEIGHT) {
    Serial.printf("[BLE] img_preview 尺寸非法: %dx%d (期望 %dx%d)\n", w, h, EPD_WIDTH, EPD_HEIGHT);
    sendError("img_preview: bad size");
    return;
  }

  size_t b64Len = strlen(b64);
  /* mbedtls_base64_decode 要求 out 缓冲 ≥ 3*b64Len/4 + 4，预留余量 */
  const size_t outCap = (b64Len * 3) / 4 + 4;
  uint8_t *buf = (uint8_t *)malloc(outCap);
  if (!buf) {
    sendError("img_preview: OOM");
    return;
  }
  size_t outLen = 0;
  int rc = mbedtls_base64_decode(buf, outCap, &outLen,
                                 (const uint8_t *)b64, b64Len);
  if (rc != 0 || outLen != EPD_BITMAP_SIZE) {
    Serial.printf("[BLE] img_preview base64 解码失败 rc=%d outLen=%u (期望 %u)\n",
                  rc, (unsigned)outLen, (unsigned)EPD_BITMAP_SIZE);
    free(buf);
    sendError("img_preview: bad b64");
    return;
  }

  epaperDrawBitmap(buf, EPD_BITMAP_SIZE);
  free(buf);
}

/**
 * img_clear 命令处理：清除 NVS 中的挂件图
 * 同样不退出配网
 */
static void handleImgClearCommand() {
  configManagerClearIdleImage();
  sendResultOk("img_clear", "");
  Serial.println("[BLE] 挂件图已清除（不退出配网）");
}

/* ============== ticket_refresh：两阶段确认 → 退出配网走 main.cpp 抓票流程 ==============
 *
 * 设计思路：不在 ble_cfg.cpp 内复制抓票逻辑，而是退出配网模式让 main.cpp 的正常流程
 * 接管（WiFi → 推密钥 → 加密 → POST /ticket → 解密 → 刷屏 → deep sleep）。
 * 避免了 WiFi/BLE 共存、代码重复等问题。
 *
 * 协议：
 *   1. 浏览器发 {"type":"ticket_refresh"}
 *   2. 设备检查 configIsComplete()，返回 {"type":"result","action":"ticket_refresh","need_confirm":true,...}
 *   3. 浏览器弹确认框（当前修改不会保存 + 蓝牙将断开）
 *   4. 用户确认 → 浏览器发 {"type":"ticket_refresh_confirm"}
 *   5. 设备设 g_bleQuit=true + g_bleResult=REFRESH → 退出配网
 *   6. main.cpp 见 REFRESH → 带 forceRefresh=true 走抓票流程 → 服务端 X-Force-Render:1
 */

/**
 * ticket_refresh 命令：检查配置完整性，请求浏览器确认
 */
static void handleTicketRefreshCommand() {
  if (!configIsComplete()) {
    sendError("ticket_refresh: 配置不完整，请先保存配置");
    return;
  }
  /* 通知浏览器：需要用户确认（当前修改不会保存，蓝牙将断开） */
  JsonDocument doc;
  doc["type"] = "result";
  doc["ok"] = true;
  doc["action"] = "ticket_refresh";
  doc["need_confirm"] = true;
  doc["message"] = "当前修改不会保存，蓝牙将断开，设备将强刷车票。是否继续？";
  String out;
  serializeJson(doc, out);
  notifyLine(out);
  Serial.println("[BLE] ticket_refresh: 已请求浏览器确认");
}

/**
 * ticket_refresh_confirm 命令：用户已确认，退出配网模式
 * 设置 g_bleResult=REFRESH，bleCfgEnter() 返回后 main.cpp 据此走抓票流程
 */
static void handleTicketRefreshConfirmCommand() {
  sendResultOk("ticket_refresh_confirm", "");
  Serial.println("[BLE] 用户确认强刷车票，退出配网模式");
  g_bleResult = CFG_RESULT_REFRESH;
  g_bleQuit = true;
}

/**
 * 解析累积到 rxBuf 的一行 JSON 命令
 */
static void parseCommandLine(const String &line) {
  if (line.length() == 0) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    Serial.printf("[BLE] JSON 解析失败: %s\n", err.c_str());
    sendError("JSON parse failed");
    return;
  }

  const char *type = doc["type"] | "";
  if (strcmp(type, "get") == 0) {
    sendCurrentConfig();
  } else if (strcmp(type, "set") == 0) {
    handleSetCommand(doc);
  } else if (strcmp(type, "reset") == 0) {
    handleResetCommand();
  } else if (strcmp(type, "img_set") == 0) {
    handleImgSetCommand(doc);
  } else if (strcmp(type, "img_clear") == 0) {
    handleImgClearCommand();
  } else if (strcmp(type, "img_preview") == 0) {
    handleImgPreviewCommand(doc);
  } else if (strcmp(type, "ticket_refresh") == 0) {
    handleTicketRefreshCommand();
  } else if (strcmp(type, "ticket_refresh_confirm") == 0) {
    handleTicketRefreshConfirmCommand();
  } else {
    sendError("unknown type");
  }
}

/* ============== NimBLE 回调 ============== */

class CfgWriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pChr) override {
    auto val = pChr->getValue();
    if (val.size() == 0) return;
    g_rxBuf.concat((const char *)val.data(), val.size());
    /* 累积到 \n 才解析（客户端会保证一帧一行） */
    int idx;
    while ((idx = g_rxBuf.indexOf('\n')) >= 0) {
      String line = g_rxBuf.substring(0, idx);
      g_rxBuf.remove(0, idx + 1);
      line.trim();
      if (line.length() > 0) {
        parseCommandLine(line);
      }
    }
  }
};

class CfgServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer) override {
    g_bleConnected = true;
    g_bleQuit = false;
    g_rxBuf = "";
    g_bleResult = CFG_RESULT_TIMEOUT;  // 每次新连接重置（防止前一次留下的 COMPLETED）
    Serial.println("[BLE] 客户端已连接");
  }
  void onDisconnect(NimBLEServer *pServer) override {
    g_bleConnected = false;
    Serial.println("[BLE] 客户端断开");
    /* 若之前没 set/reset 成功过，这次断开算"用户取消"——
       区分于"5 分钟真没人连"的 TIMEOUT，main.cpp 行为相同但便于日志诊断 */
    if (g_bleResult == CFG_RESULT_TIMEOUT) {
      g_bleResult = CFG_RESULT_CANCELED;
    }
    /* 客户端断开 → 退出配网（超时靠外层 while 兜底） */
    g_bleQuit = true;
  }
};

/* ============== 公开 API ============== */

/**
 * 进入配网模式（阻塞，直到连接断开/超时/配置成功）
 * @param waitConnMs 等待客户端连接的窗口时长：
 *   - 0   ：整体 CFG_BLE_TIMEOUT_MS（5 分钟）超时（兼容旧逻辑）
 *   - >0  ：先等 waitConnMs；窗口内无人连接返回 NO_CONNECT；
 *           有人连接则启动 CFG_BLE_TIMEOUT_MS 会话超时
 * @return 退出原因（CfgResult）：
 *   - CFG_RESULT_TIMEOUT   会话超时（waitConnMs=0 时为整体 5 分钟；waitConnMs>0 时为连接后 5 分钟）
 *   - CFG_RESULT_CANCELED  连过但没保存就断开
 *   - CFG_RESULT_COMPLETED set/reset 成功落盘
 *   - CFG_RESULT_NO_CONNECT 等待窗口内无人连接（仅 waitConnMs > 0）
 * 退出后调用方根据 result 决定 esp_restart（COMPLETED）、
 * deepSleepUntil（TIMEOUT/CANCELED）或继续抓票（NO_CONNECT）
 */
CfgResult bleCfgEnter(uint32_t waitConnMs) {
  Serial.println("[BLE] 初始化 NimBLE...");
  /* BLE 与 WiFi 共存有冲突，配网时强制关 WiFi（先 disconnect 再 mode OFF） */
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  NimBLEDevice::init(CFG_BLE_DEV_NAME);
  /* 主动设大 preferred MTU：让 NimBLE 在收到客户端 MTU Exchange 时按
     上限响应（Web Bluetooth 通常会请求 247，但服务端不显式声明会被
     某些实现保守地按 23 处理，导致 notify 单包仅 20 字节，
     configManagerDumpJson() 的 ~280B JSON 必然被静默丢弃） */
  NimBLEDevice::setMTU(512);

  NimBLEServer *pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new CfgServerCallbacks());

  NimBLEService *pSvc = pServer->createService(CFG_BLE_SVC_UUID);

  /* 写配置：用 WRITE_NR（write-without-response）而非 WRITE。
     原因：Web Bluetooth 规范禁止浏览器在 buffer > MTU-3 时自动用 Long Write 协议
     （NimBLE peripheral 角色也默认关闭 BLE_GATT_WRITE_LONG，见 ble_gattc.c:3822），
     而 Chromium 在 Windows 上的 writeValueWithResponse 存在已知 bug：单帧 > 20B
     即报 "GATT operation failed for unknown reason"，且多次写期间固件若在
     同步 notify result 会与 ACK 在 NimBLE 单线程 GATT 队列里竞争失败。
     WRITE_NR 不需要 ACK，单帧 20B 协议稳，配合固件端 g_rxBuf 流式累积（按 \n
     解析）即可正确重组整段 JSON；可靠性靠 ESP32 ↔ 浏览器短距离几乎不丢包保证 */
  g_chrW = pSvc->createCharacteristic(
      CFG_BLE_CHR_WRITE_UUID,
      NIMBLE_PROPERTY::WRITE_NR);
  g_chrW->setCallbacks(new CfgWriteCallbacks());

  /* 通知：服务端响应推送（max 必须 >= 一帧 JSON，否则 setValue 失败） */
  g_chrN = pSvc->createCharacteristic(
      CFG_BLE_CHR_NOTIFY_UUID,
      NIMBLE_PROPERTY::NOTIFY,
      512);
  /* NOTIFY characteristic 写入首字节触发 CCCD 注册 */
  g_chrN->setValue((uint8_t *)" ", 1);

  pSvc->start();

  NimBLEAdvertising *pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(CFG_BLE_SVC_UUID);
  /* 让客户端更快发现 */
  pAdv->setScanResponse(true);
  pAdv->setMinPreferred(0x06);
  pAdv->setMaxPreferred(0x12);
  pAdv->start();

  g_rxBuf = "";
  g_bleQuit = false;
  g_bleConnected = false;
  g_bleResult = CFG_RESULT_TIMEOUT;  // 初始值：若整个超时循环都没人连，就是 TIMEOUT

  if (waitConnMs > 0) {
    Serial.printf("[BLE] 广播中（设备名=%s），等待连接（%u 秒窗口 + 连接后 %u 分钟会话）...\n",
                  CFG_BLE_DEV_NAME,
                  (unsigned)(waitConnMs / 1000),
                  (unsigned)(CFG_BLE_TIMEOUT_MS / 60000));
  } else {
    Serial.printf("[BLE] 广播中（设备名=%s），等待连接（整体 %u 分钟超时）...\n",
                  CFG_BLE_DEV_NAME,
                  (unsigned)(CFG_BLE_TIMEOUT_MS / 60000));
  }

  uint32_t start = millis();
  /* connectedOnce / connStart：阶段 2（连接后会话超时）的起点
     waitConnMs>0 时，首次连接触发会话计时；waitConnMs=0 时不用 */
  bool connectedOnce = false;
  uint32_t connStart = 0;
  while (true) {
    uint32_t now = millis();

    /* 首次连接：启动会话超时计时 */
    if (g_bleConnected && !connectedOnce) {
      connectedOnce = true;
      connStart = now;
      if (waitConnMs > 0) {
        Serial.println("[BLE] 客户端首次连接，启动 5 分钟会话超时");
      }
    }

    /* 配网完成 / 客户端断开 → 等客户端断开后退出（保证最后一条 notify 投递） */
    if (g_bleQuit) {
      if (g_bleConnected) {
        delay(2000);
      } else {
        break;
      }
    }

    /* 阶段 1：等待窗口超时（仅 waitConnMs>0 且尚未连接时检查）
       无人连接 → 返回 NO_CONNECT，调用方继续抓票流程 */
    if (waitConnMs > 0 && !connectedOnce && (now - start) >= waitConnMs) {
      Serial.println("[BLE] 等待连接窗口超时，无人连接，退出配网模式");
      g_bleResult = CFG_RESULT_NO_CONNECT;
      break;
    }

    /* 阶段 2：会话超时
       - waitConnMs>0：连接后 CFG_BLE_TIMEOUT_MS（5 分钟）
       - waitConnMs=0：整体 CFG_BLE_TIMEOUT_MS（5 分钟，兼容旧行为） */
    bool sessionExpired = false;
    if (waitConnMs > 0) {
      if (connectedOnce && (now - connStart) >= CFG_BLE_TIMEOUT_MS) {
        sessionExpired = true;
      }
    } else {
      if ((now - start) >= CFG_BLE_TIMEOUT_MS) {
        sessionExpired = true;
      }
    }
    if (sessionExpired) {
      Serial.println("[BLE] 会话超时");
      break;
    }

    delay(50);
  }
  /* 退出主循环的情况：
   * 1) g_bleQuit=true → g_bleResult 已被 set/reset 设为 COMPLETED，
   *    或被 onDisconnect 升级为 CANCELED
   * 2) waitConnMs>0 且无人连接 → NO_CONNECT
   * 3) 会话超时 → 保持 CFG_RESULT_TIMEOUT */

  const char *tag =
      (g_bleResult == CFG_RESULT_COMPLETED)  ? "COMPLETED"  :
      (g_bleResult == CFG_RESULT_CANCELED)   ? "CANCELED"   :
      (g_bleResult == CFG_RESULT_NO_CONNECT) ? "NO_CONNECT" :
      (g_bleResult == CFG_RESULT_REFRESH)    ? "REFRESH"    : "TIMEOUT";
  Serial.printf("[BLE] 退出配网模式（%s），关闭广播\n", tag);

  NimBLEDevice::deinit(false);
  g_chrW = nullptr;
  g_chrN = nullptr;
  delay(200);
  return g_bleResult;
}
