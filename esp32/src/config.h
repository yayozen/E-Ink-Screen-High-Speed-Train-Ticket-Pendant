/**
 * 全局配置与引脚定义
 * 集中放置 WiFi、服务端地址、墨水屏引脚、IMAP 加密参数、唤醒时间
 *
 * v4 方案：
 *   - 票务服务端地址 = 完整 URL（http://host:port/path），有默认
 *   - WiFi / IMAP / 唤醒时间：NVS 缺失即未配，启动时强制进入配网模式
 *   - 显示模式 / WiFi 超时 / SSL：固定为 #define，不暴露给用户
 *   - 配网模式：上电时启动 30 秒 BLE 等待窗口；缺配置时直接进入 5 分钟配网
 */
#pragma once

#include <Arduino.h>
#include <esp_system.h>
#include <stdint.h>

/* ============================================================
 * 运行时可配项
 * ============================================================ */

/* WiFi（必需，无默认）
 * 支持多组：第 0 组为必需（缺即进配网），其余为可选备用
 * connectWifi() 会按顺序逐个尝试，连上即返回 */
#define WIFI_ENTRIES_MAX  3

struct WifiEntry {
  char ssid[33];
  char pass[65];
};

extern WifiEntry WIFI_ENTRIES[WIFI_ENTRIES_MAX];

/* 票务服务端完整 URL（含 http(s):// + host + port + path）
 * TICKET_URL 为主（有默认），TICKET_URL_1 为灾备（可选，留空表示无灾备）
 * 设备按 主 → 灾备 顺序回退，任一可用即用 */
extern char TICKET_URL[128];
extern char TICKET_URL_1[128];

/* IMAP（必需，无默认；密码落 NVS 时用 AES 加密） */
extern char IMAP_USER[65];
extern char IMAP_PASS[65];
extern char IMAP_HOST[65];
extern uint16_t IMAP_PORT;

/* 唤醒时间（必需；255 = 未配，由 configIsComplete 检查） */
extern uint8_t WAKE_HOUR;
extern uint8_t WAKE_MINUTE;

/* AES 共享密钥（必需；32 字符 hex + '\0'，对应 16 字节 AES-128）
 * 首次配对前为空串；配对时由用户在 BLE 配网页面生成或填写，固件端落 NVS、
 * 推送给服务端（POST /key）；抓票时不再带密钥 */
extern char AES_KEY[33];

/* ============================================================
 * 不可配（硬件/策略决定，写死）
 * ============================================================ */
#define WIFI_TIMEOUT_MS         8000UL     // 单组 WiFi 连接超时（最坏 N×此值；原 10s 过长）
#define IMAP_USE_SECURE         true       // IMAP 强制 SSL/TLS
#define DISPLAY_ALWAYS_FULL_REFRESH 1      // 每次唤醒都全刷（1=是，0=内容变化才全刷）
#define TICKET_TIMEOUT_MS       8000UL     // HTTP /ticket 请求超时（原 15s；服务端 IMAP 抓取实际 3-5s，留余量即可）

/* ============================================================
 * 默认值实现：仅 TICKET_URL 有出厂默认；其他字段在 NVS 缺失时保持"空/未配"
 * ============================================================ */
void configLoadDefaults();

/**
 * 检查所有必需字段是否已配置（WiFi / IMAP / 唤醒时间）
 * @return true 全部齐，false 有缺失（应进入配网）
 */
bool configIsComplete();

/* ============================================================
 * 设备 ID：从 ESP32-D0WDQ6（V3 版本） eFuse MAC 派生
 * ============================================================ */
inline String makeDeviceId() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[24];
  snprintf(buf, sizeof(buf), "CT32-%04X%08X",
           (uint16_t)(mac >> 32), (uint32_t)mac);
  return String(buf);
}

/* ============================================================
 * 同步标志：用于在 deep sleep 唤醒后判断是否需要刷屏
 * ============================================================ */
#define RTC_MAGIC     0xC0DE

/* ============================================================
 * 配网触发（v6：启动时窗口式等待，不再使用 BOOT 长按）
 *   - 上电时若 configIsComplete() == false → 立即进入配网（5 分钟整体超时）
 *   - 配置完整时，setup 启动后开 30 秒 BLE 等待窗口：
 *       无人连接 → 继续抓票；有人连接 → 进入 5 分钟会话超时
 *   - BOOT 按钮 GPIO 仅作为硬件参考定义保留（当前代码不再读取）
 *       ESP32-DevKitC     → GPIO0
 *       ESP32-C3 SuperMini → GPIO9
 * ============================================================ */
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  #define CFG_BTN_GPIO        9
#else
  #define CFG_BTN_GPIO        0
#endif
#define CFG_BTN_ACTIVE_LOW  1

/* ============================================================
 * BLE 配网 GATT UUID（自生成 128-bit，避免与常见示例冲突）
 * ============================================================ */
#define CFG_BLE_SVC_UUID        "4a4b5c6d-1111-2222-3333-444455556666"
#define CFG_BLE_CHR_WRITE_UUID  "4a4b5c6d-1111-2222-3333-aaaabbbbcccc"
#define CFG_BLE_CHR_NOTIFY_UUID "4a4b5c6d-1111-2222-3333-111122223333"
#define CFG_BLE_DEV_NAME        "TicketBadge-Cfg"

/* ============================================================
 * 屏幕硬件（不可配，按物理接线 + 屏幕型号硬编码）
 *   - ESP32-DevKitC：VSPI 默认引脚 SCK=18/MOSI=23 + CS=5/DC=17/RST=16/BUSY=4
 *   - ESP32-C3 SuperMini：避开 USB(18/19)/Flash(12-17)/BOOT(9)/LED(8)/strapping(2)，
 *     选用 GPIO3/4/5/6/7/10；如实际接线不同请同步修改此处
 *   - Seeed XIAO ESP32-S3：丝印-实际 GPIO 映射见 docs/硬件接线.md
 *       SCK=D8=7 / MOSI=D10=9 / CS=D4=5 / DC=D2=3 / RST=D3=4 / BUSY=D1=2
 * ============================================================ */
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  #define EPD_CS        7
  #define EPD_DC        10
  #define EPD_RST       3
  #define EPD_BUSY      5
  #define EPD_MOSI      6
  #define EPD_SCK       4
#elif defined(IS_xIAO_ESP32S3)
  #define EPD_CS        5
  #define EPD_DC        3
  #define EPD_RST       1
  // 驱动板busy引脚损坏改成-1, 实际引脚为2。 rst bad 1, ok 4
  #define EPD_BUSY      -1
  #define EPD_MOSI      9
  #define EPD_SCK       7
#else
  #define EPD_CS        5
  #define EPD_DC        17
  #define EPD_RST       16
  // 驱动板busy引脚损坏改成-1, 实际引脚为4
  #define EPD_BUSY      -1
  #define EPD_SCK       18
  #if defined(IS_ESP32S3)
    #define EPD_MOSI      21
  #else
    #define EPD_MOSI      23
  #endif
#endif

/* 屏幕逻辑尺寸：与 GxEPD2 驱动绑定，编译时确定
 *   RWB_SCREEN   → GxEPD2_213_Z98c（250×122 横屏）
 *   默认（BN 屏）→ GxEPD2_213_BN  （212×104 横屏）
 * 服务端通过 X-Screen-W/X-Screen-H header 获知此尺寸，自动缩放位图 */
#if defined(SCREEN_212)
  #define EPD_WIDTH     212
  #define EPD_HEIGHT    104
#else
  #define EPD_WIDTH     250
  #define EPD_HEIGHT    122
#endif
