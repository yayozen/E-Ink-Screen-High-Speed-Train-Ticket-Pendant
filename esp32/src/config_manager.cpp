/**
 * 配置持久化管理（v4）
 *
 * NVS namespace = "cfg"
 *   字段（精简后）：
 *     wf_ssid / wf_pass                WiFi 主（entry 0）
 *     wf_ssid_1 / wf_pass_1            WiFi 备用 1（entry 1，可空）
 *     wf_ssid_2 / wf_pass_2            WiFi 备用 2（entry 2，可空）
 *     tk_url                           票务服务端完整 URL（有默认，NVS 缺失也走得通）
 *     im_user / im_pass (AES) / im_host / im_port   IMAP（pass 用 AES 加密）
 *     wk_h / wk_m                      唤醒时间（255 = 未配）
 *
 * 协议（来自 BLE 配网页面，wifi 支持对象或数组两种形式）：
 *   { "type":"set",
 *     "wifi":   { "ssid":"...", "pass":"..." } | [{ssid,pass}, ...],
 *     "ticket": { "url":"http://host:port/path", "url2":"http://dr:port/path" },
 *     "imap":   { "user":"...", "pass":"...", "host":"...", "port":993 },
 *     "wake":   { "hour":1, "minute":30 } }
 */
#include "config_manager.h"
#include "config.h"
#include "epaper_render.h"   /* EPD_BITMAP_SIZE（挂件图长度校验） */
#include "utils.h"
#include <Preferences.h>
#include <string.h>

/* NVS 命名空间 */
static const char *NVS_NS = "cfg";

/* 字段名（保持稳定，避免改名导致丢配置）
 * entry 0 沿用旧 key（wf_ssid/wf_pass），保证旧设备升级后配置不丢；
 * entry 1/2 用 _1/_2 后缀 */
static const char *K_WIFI_SSID[WIFI_ENTRIES_MAX] = { "wf_ssid", "wf_ssid_1", "wf_ssid_2" };
static const char *K_WIFI_PASS[WIFI_ENTRIES_MAX] = { "wf_pass", "wf_pass_1", "wf_pass_2" };
static const char *K_TICKET_URL    = "tk_url";
static const char *K_TICKET_URL_1  = "tk_url_1";   /* 灾备服务端（可选） */
static const char *K_IMAP_USER    = "im_user";
static const char *K_IMAP_PASS    = "im_pass";   /* AES 加密后的 base64 */
static const char *K_IMAP_HOST    = "im_host";
static const char *K_IMAP_PORT    = "im_port";
static const char *K_WAKE_HOUR    = "wk_h";
static const char *K_WAKE_MIN     = "wk_m";
static const char *K_AES_KEY      = "tk_aes";    /* 32 字符 hex 明文存（与 IMAP_PASS 同样标准） */
static const char *K_KEY_PUSHED   = "kp";        /* 密钥已推送标志（bool，true=已推送） */
static const char *K_PENDANT_IMG  = "pb_img";    /* 挂件图（无票时显示），1bit binary blob 3904B */

/* ============== 内部：NVS 读写 ============== */

static void nvsReadString(Preferences &p, const char *key, char *dst, size_t dstSize) {
  String s = p.getString(key, "");
  if (s.length() == 0) return;
  strncpy(dst, s.c_str(), dstSize - 1);
  dst[dstSize - 1] = '\0';
}

/**
 * 读 AES 加密字符串并解密回明文（使用运行时 AES_KEY）
 * 解不出来（密钥未配 / 密钥已变更）→ 保持 dst 为空，让用户重新配置
 */
static void nvsReadEncrypted(Preferences &p, const char *key, char *dst, size_t dstSize) {
  String enc = p.getString(key, "");
  if (enc.length() == 0) return;
  if (AES_KEY[0] == '\0') {
    /* 首次启动 / 刚恢复出厂：NVS 里残留的密文先不解，避免拿错密钥解密 */
    return;
  }
  String dec = aesDecryptBase64(enc, AES_KEY);
  if (dec.length() == 0) {
    /* 解密失败：AES_KEY 可能已变更（用户重新配对），旧密文无法解密。
       不把密文当明文 fallback（会导致 IMAP 登录失败），保持 dst 为空让用户重新配置 */
    Serial.println("[CFG] NVS 密文解密失败，保持空值（密钥可能已变更）");
    return;
  }
  strncpy(dst, dec.c_str(), dstSize - 1);
  dst[dstSize - 1] = '\0';
}

/* ============== 公开 API ============== */

/**
 * 启动：先填默认值，再用 NVS 中存在的项覆盖
 */
void configManagerBegin() {
  configLoadDefaults();

  Preferences p;
  if (!p.begin(NVS_NS, true)) {
    Serial.println("[CFG] NVS 打开失败，仅使用默认值");
    return;
  }

  /* WiFi 多组：entry 0 必需，1/2 可选 */
  for (int i = 0; i < WIFI_ENTRIES_MAX; i++) {
    nvsReadString(p, K_WIFI_SSID[i], WIFI_ENTRIES[i].ssid, sizeof(WIFI_ENTRIES[i].ssid));
    nvsReadString(p, K_WIFI_PASS[i], WIFI_ENTRIES[i].pass, sizeof(WIFI_ENTRIES[i].pass));
  }

  nvsReadString(p, K_TICKET_URL,    TICKET_URL,    sizeof(TICKET_URL));
  nvsReadString(p, K_TICKET_URL_1,  TICKET_URL_1,  sizeof(TICKET_URL_1));

  nvsReadString(p, K_IMAP_USER,   IMAP_USER,   sizeof(IMAP_USER));
  nvsReadString(p, K_IMAP_HOST,   IMAP_HOST,   sizeof(IMAP_HOST));
  IMAP_PORT = p.getUShort(K_IMAP_PORT, IMAP_PORT);

  /* 唤醒时间：NVS 缺失时保留默认的 255（未配） */
  if (p.isKey(K_WAKE_HOUR)) {
    int h = p.getUChar(K_WAKE_HOUR, 255);
    if (h <= 23) WAKE_HOUR = (uint8_t)h;
  }
  if (p.isKey(K_WAKE_MIN)) {
    int m = p.getUChar(K_WAKE_MIN, 255);
    if (m <= 59) WAKE_MINUTE = (uint8_t)m;
  }

  /* AES 共享密钥：NVS 缺失时保持空（configIsComplete 检查长度 = 32） */
  nvsReadString(p, K_AES_KEY, AES_KEY, sizeof(AES_KEY));

  // 解密放在aeskey后，确保aeskey已加载
  nvsReadEncrypted(p, K_IMAP_PASS, IMAP_PASS,  sizeof(IMAP_PASS));

  p.end();
  Serial.printf("[CFG] 加载完成 wifi[0]=%s wifi[1]=%s wifi[2]=%s url=%s dr=%s imap=%s@%s wake=%02u:%02u aesKeyLen=%u complete=%s\n",
                WIFI_ENTRIES[0].ssid, WIFI_ENTRIES[1].ssid, WIFI_ENTRIES[2].ssid,
                TICKET_URL, TICKET_URL_1, IMAP_USER, IMAP_HOST,
                WAKE_HOUR, WAKE_MINUTE, (unsigned)strlen(AES_KEY),
                configIsComplete() ? "YES" : "NO");
}

/**
 * 清空 NVS 全部键并把内存重置为默认值
 */
void configManagerResetToDefaults() {
  Preferences p;
  if (p.begin(NVS_NS, false)) {
    p.clear();
    p.end();
  }
  configLoadDefaults();
  /* 挂件图也算"出厂内容"的一部分，一并清空（用户从 BLE 配网页面"恢复出厂"） */
  configManagerClearIdleImage();
  Serial.println("[CFG] 已恢复出厂默认值（含挂件图）");
}

/**
 * 把当前内存变量落盘
 */
void configManagerSaveAll() {
  Preferences p;
  if (!p.begin(NVS_NS, false)) {
    Serial.println("[CFG] NVS 写打开失败");
    return;
  }

  /* WiFi 多组：把已填的写入，超过实际配置数（用户减少 WiFi 后）清除残留 */
  for (int i = 0; i < WIFI_ENTRIES_MAX; i++) {
    if (WIFI_ENTRIES[i].ssid[0] != '\0') {
      p.putString(K_WIFI_SSID[i], WIFI_ENTRIES[i].ssid);
      /* pass 允许为空（开放网络）：ssid 在但 pass 空时仍写入空串 */
      p.putString(K_WIFI_PASS[i], WIFI_ENTRIES[i].pass);
    } else {
      /* ssid 空 → 此槽位未用，清掉 NVS 残留避免下次读到旧值 */
      p.remove(K_WIFI_SSID[i]);
      p.remove(K_WIFI_PASS[i]);
    }
  }

  p.putString(K_TICKET_URL,    TICKET_URL);
  /* 灾备 URL：留空时清掉 NVS，避免下次读到旧值 */
  if (TICKET_URL_1[0] != '\0') {
    p.putString(K_TICKET_URL_1, TICKET_URL_1);
  } else {
    p.remove(K_TICKET_URL_1);
  }

  p.putString(K_IMAP_USER, IMAP_USER);
  /* IMAP_PASS 用 AES_KEY 加密后存 base64；密钥未配时不写（避免明文落盘） */
  if (AES_KEY[0] != '\0' && IMAP_PASS[0] != '\0') {
    String encPass = aesEncryptBase64(String(IMAP_PASS), AES_KEY);
    if (encPass.length() > 0) {
      p.putString(K_IMAP_PASS, encPass);
    } else {
      p.remove(K_IMAP_PASS);
    }
  } else {
    p.remove(K_IMAP_PASS);
  }
  p.putString(K_IMAP_HOST, IMAP_HOST);
  p.putUShort(K_IMAP_PORT, IMAP_PORT);

  p.putUChar(K_WAKE_HOUR, WAKE_HOUR);
  p.putUChar(K_WAKE_MIN,  WAKE_MINUTE);

  p.putString(K_AES_KEY, AES_KEY);

  /* 关键：任何一次落盘（BLE set / 恢复出厂）都意味着配置被改动，
     必须清除"密钥已推送"标志 → 下次 setup 启动会重新执行 POST /key，
     保证服务端 config.aesKeys[deviceId] 始终与本机 NVS 一致 */
  p.putBool(K_KEY_PUSHED, false);

  p.end();
  Serial.println("[CFG] 全部可配项已落盘（IMAP_PASS 已加密，kp/fc 已重置）");
}

/**
 * 把 JSON 应用到内存并落盘；返回成功应用的字段名（点号路径）
 */
String configManagerApplyJson(const JsonDocument &doc) {
  String updated;
  updated.reserve(128);

  /* wifi
   * 兼容两种格式：
   *   1) 对象：{"wifi":{"ssid":"...","pass":"..."}}  → 只设 entry 0，其余清空
   *   2) 数组：{"wifi":[{"ssid":"...","pass":"..."},...]}  → 替换所有条目（多余槽位清空）
   * 数组格式下发长度 ≤ WIFI_ENTRIES_MAX 时，剩余槽位会被清空 */
  if (doc["wifi"].is<JsonArrayConst>()) {
    JsonArrayConst arr = doc["wifi"].as<JsonArrayConst>();
    int idx = 0;
    for (JsonObjectConst we : arr) {
      if (idx >= WIFI_ENTRIES_MAX) break;
      if (we["ssid"].is<const char *>()) {
        strncpy(WIFI_ENTRIES[idx].ssid, we["ssid"].as<const char *>(),
                sizeof(WIFI_ENTRIES[idx].ssid) - 1);
        WIFI_ENTRIES[idx].ssid[sizeof(WIFI_ENTRIES[idx].ssid) - 1] = '\0';
        updated += "wifi[" + String(idx) + "].ssid,";
      }
      /* pass 字段缺省 = 不修改；显式空串 = 清空（开放网络） */
      if (we["pass"].is<const char *>()) {
        strncpy(WIFI_ENTRIES[idx].pass, we["pass"].as<const char *>(),
                sizeof(WIFI_ENTRIES[idx].pass) - 1);
        WIFI_ENTRIES[idx].pass[sizeof(WIFI_ENTRIES[idx].pass) - 1] = '\0';
        updated += "wifi[" + String(idx) + "].pass,";
      }
      idx++;
    }
    /* 数组格式下发少于 WIFI_ENTRIES_MAX → 清空后面的槽位（用户删了备用 WiFi） */
    while (idx < WIFI_ENTRIES_MAX) {
      if (WIFI_ENTRIES[idx].ssid[0] != '\0') {
        WIFI_ENTRIES[idx].ssid[0] = '\0';
        WIFI_ENTRIES[idx].pass[0] = '\0';
      }
      idx++;
    }
  } else if (doc["wifi"].is<JsonObjectConst>()) {
    JsonObjectConst w = doc["wifi"].as<JsonObjectConst>();
    /* 对象格式：等价于只设 entry 0；其余清空 */
    for (int i = 1; i < WIFI_ENTRIES_MAX; i++) {
      WIFI_ENTRIES[i].ssid[0] = '\0';
      WIFI_ENTRIES[i].pass[0] = '\0';
    }
    if (w["ssid"].is<const char *>()) {
      strncpy(WIFI_ENTRIES[0].ssid, w["ssid"].as<const char *>(),
              sizeof(WIFI_ENTRIES[0].ssid) - 1);
      WIFI_ENTRIES[0].ssid[sizeof(WIFI_ENTRIES[0].ssid) - 1] = '\0';
      updated += "wifi[0].ssid,";
    }
    if (w["pass"].is<const char *>()) {
      strncpy(WIFI_ENTRIES[0].pass, w["pass"].as<const char *>(),
              sizeof(WIFI_ENTRIES[0].pass) - 1);
      WIFI_ENTRIES[0].pass[sizeof(WIFI_ENTRIES[0].pass) - 1] = '\0';
      updated += "wifi[0].pass,";
    }
  }

  /* ticket URL
   * - url    必传，固件照单全收（含空串 = 清空主 URL → 触发 CONFIG REQUIRED）
   * - url2   可选灾备；缺省/空 = 不修改 NVS 中已有的灾备 URL（与 WiFi 行为一致） */
  if (doc["ticket"].is<JsonObjectConst>()) {
    JsonObjectConst t = doc["ticket"].as<JsonObjectConst>();
    if (t["url"].is<const char *>()) {
      strncpy(TICKET_URL, t["url"].as<const char *>(), sizeof(TICKET_URL) - 1);
      TICKET_URL[sizeof(TICKET_URL) - 1] = '\0';
      updated += "ticket.url,";
    }
    if (t["url2"].is<const char *>()) {
      strncpy(TICKET_URL_1, t["url2"].as<const char *>(), sizeof(TICKET_URL_1) - 1);
      TICKET_URL_1[sizeof(TICKET_URL_1) - 1] = '\0';
      updated += "ticket.url2,";
    }
  }

  /* imap */
  JsonObjectConst i = doc["imap"].as<JsonObjectConst>();
  if (!i.isNull()) {
    if (i["user"].is<const char *>()) {
      strncpy(IMAP_USER, i["user"].as<const char *>(), sizeof(IMAP_USER) - 1);
      IMAP_USER[sizeof(IMAP_USER) - 1] = '\0';
      updated += "imap.user,";
    }
    if (i["pass"].is<const char *>()) {
      strncpy(IMAP_PASS, i["pass"].as<const char *>(), sizeof(IMAP_PASS) - 1);
      IMAP_PASS[sizeof(IMAP_PASS) - 1] = '\0';
      updated += "imap.pass,";
    }
    if (i["host"].is<const char *>()) {
      strncpy(IMAP_HOST, i["host"].as<const char *>(), sizeof(IMAP_HOST) - 1);
      IMAP_HOST[sizeof(IMAP_HOST) - 1] = '\0';
      updated += "imap.host,";
    }
    if (i["port"].is<uint16_t>() || i["port"].is<int>()) {
      IMAP_PORT = i["port"].as<uint16_t>();
      updated += "imap.port,";
    }
  }

  /* wake */
  JsonObjectConst wkw = doc["wake"].as<JsonObjectConst>();
  if (!wkw.isNull()) {
    if (wkw["hour"].is<int>()) {
      int h = wkw["hour"].as<int>();
      if (h >= 0 && h <= 23) {
        WAKE_HOUR = (uint8_t)h;
        updated += "wake.hour,";
      }
    }
    if (wkw["minute"].is<int>()) {
      int m = wkw["minute"].as<int>();
      if (m >= 0 && m <= 59) {
        WAKE_MINUTE = (uint8_t)m;
        updated += "wake.minute,";
      }
    }
  }

  /* AES 共享密钥（必须是 32 字符 hex） */
  if (doc["aesKey"].is<const char *>()) {
    String hex = doc["aesKey"].as<const char *>();
    if (hex.length() == 32) {
      strncpy(AES_KEY, hex.c_str(), sizeof(AES_KEY) - 1);
      AES_KEY[sizeof(AES_KEY) - 1] = '\0';
      updated += "aes.key,";
    } else {
      Serial.printf("[CFG] aesKey 长度非法: %u (期望 32)\n", (unsigned)hex.length());
    }
  }

  configManagerSaveAll();
  return updated;
}

/**
 * 把当前配置导出为 JSON（不含密码 / 密钥脱敏）
 */
void configManagerDumpJson(JsonDocument &out) {
  String pass_overlay = "****************";
  out["deviceId"] = makeDeviceId();
  /* wifi 输出为数组：浏览器端 fillForm 按索引填到对应输入框 */
  JsonArray wa = out["wifi"].to<JsonArray>();
  for (int i = 0; i < WIFI_ENTRIES_MAX; i++) {
    JsonObject we = wa.add<JsonObject>();
    we["ssid"] = WIFI_ENTRIES[i].ssid;
    we["pass"] = "";   // 永远不回显密码
    if (WIFI_ENTRIES[i].pass[0] != '\0') {
      we["pass"] = pass_overlay;
    }
  }
  JsonObject to = out["ticket"].to<JsonObject>();
  to["url"]  = TICKET_URL;
  to["url2"] = TICKET_URL_1;
  JsonObject io = out["imap"].to<JsonObject>();
  io["user"] = IMAP_USER;
  io["pass"] = "";
  if (IMAP_PASS[0] != '\0') {
    io["pass"] = pass_overlay;
  }
  io["host"] = IMAP_HOST;
  io["port"] = IMAP_PORT;
  JsonObject wko = out["wake"].to<JsonObject>();
  wko["hour"]   = WAKE_HOUR;
  wko["minute"] = WAKE_MINUTE;
  /* 密钥脱敏：已配时显示前 4 + 末 4，中间 *** */
  String masked;
  if (AES_KEY[0] != '\0') {
    masked = String(AES_KEY).substring(0, 4) + "****" + String(AES_KEY).substring(28);
  }
  out["aesKey"] = masked;
  /* 挂件图只回报"有/无"，不回传 bitmap 本身（避免响应超 MTU） */
  out["idleImage"] = configManagerHasIdleImage();
  out["complete"] = configIsComplete();
}

/* ============== 密钥推送标志位 ============== */

/**
 * 查 NVS 中 kp 标志
 * - 缺键 / 读失败 → false（视为"未推送"，触发 POST /key）
 * - 显式 true      → true（已推送，跳过）
 */
bool configManagerIsKeyPushed() {
  Preferences p;
  if (!p.begin(NVS_NS, true)) {
    return false;
  }
  bool v = p.getBool(K_KEY_PUSHED, false);
  p.end();
  return v;
}

/**
 * POST /key 成功后由 main.cpp 调用，把 kp 置 true
 * 下次 setup 启动据此跳过推送（避免每日数十次重复推送同一密钥）
 */
void configManagerMarkKeyPushed() {
  Preferences p;
  if (!p.begin(NVS_NS, false)) {
    Serial.println("[CFG] kp 标志写入失败");
    return;
  }
  p.putBool(K_KEY_PUSHED, true);
  p.end();
}

/**
 * 把 kp 标志重置为 false（服务端 datas.json 误删 / 401 设备未注册时调用）
 * 下次 setup 启动会重新走 POST /key 流程，自动恢复双方密钥一致
 */
void configManagerResetKeyPushed() {
  Preferences p;
  if (!p.begin(NVS_NS, false)) {
    Serial.println("[CFG] kp 标志重置失败");
    return;
  }
  p.putBool(K_KEY_PUSHED, false);
  p.end();
  Serial.println("[CFG] kp 已重置为 false（下次启动将重新推送密钥）");
}

/* ============== 挂件图（idle image）存取 ============== */

/**
 * 查询 NVS 是否已存有挂件图
 * 用 getBytesLength 探测 key 存在且长度 == EPD_BITMAP_SIZE 才算"完整"
 * （防御性检查：NVS 因掉电/写入失败可能残留部分数据）
 */
bool configManagerHasIdleImage() {
  Preferences p;
  if (!p.begin(NVS_NS, true)) {
    return false;
  }
  size_t len = p.getBytesLength(K_PENDANT_IMG);
  p.end();
  return (len == EPD_BITMAP_SIZE);
}

/**
 * 把 1bit 位图写入 NVS（覆盖式）
 * 直接 putBytes，不加密（挂件图为公开内容）
 */
bool configManagerSaveIdleImage(const uint8_t *bitmap, size_t len) {
  if (!bitmap || len != EPD_BITMAP_SIZE) {
    Serial.printf("[IMG] 保存失败：参数非法 (len=%u, expect=%u)\n",
                  (unsigned)len, (unsigned)EPD_BITMAP_SIZE);
    return false;
  }
  Preferences p;
  if (!p.begin(NVS_NS, false)) {
    Serial.println("[IMG] NVS 写打开失败");
    return false;
  }
  size_t written = p.putBytes(K_PENDANT_IMG, bitmap, len);
  p.end();
  if (written != len) {
    Serial.printf("[IMG] NVS 写入不完整: %u/%u\n", (unsigned)written, (unsigned)len);
    return false;
  }
  Serial.printf("[IMG] 挂件图已保存: %u 字节\n", (unsigned)written);
  return true;
}

/**
 * 从 NVS 读出 1bit 位图
 * 失败时不清空 buffer（让调用方自己决定）
 */
bool configManagerLoadIdleImage(uint8_t *bitmap, size_t len) {
  if (!bitmap || len != EPD_BITMAP_SIZE) {
    Serial.printf("[IMG] 读取失败：参数非法 (len=%u, expect=%u)\n",
                  (unsigned)len, (unsigned)EPD_BITMAP_SIZE);
    return false;
  }
  if (!configManagerHasIdleImage()) {
    return false;
  }
  Preferences p;
  if (!p.begin(NVS_NS, true)) {
    return false;
  }
  size_t read = p.getBytes(K_PENDANT_IMG, bitmap, len);
  p.end();
  if (read != len) {
    Serial.printf("[IMG] NVS 读取不完整: %u/%u\n", (unsigned)read, (unsigned)len);
    return false;
  }
  return true;
}

/**
 * 从 NVS 删除挂件图 key
 * 静默失败：key 不存在时 remove 也不会报错
 */
void configManagerClearIdleImage() {
  Preferences p;
  if (!p.begin(NVS_NS, false)) {
    return;
  }
  p.remove(K_PENDANT_IMG);
  p.end();
  Serial.println("[IMG] 挂件图已清除");
}
