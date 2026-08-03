/**
 * 配置默认值
 *
 * 规则（v4）：
 *   - 仅 TICKET_URL 有出厂默认（其他字段无默认）
 *   - WiFi / IMAP / 唤醒时间：NVS 缺失时保持空串 / 0，触发强制配网
 *   - 唤醒时间 WAKE_HOUR/WAKE_MINUTE = 255 表示"未配"（configIsComplete 检查 < 24/60）
 */
#include "config.h"
#include <string.h>

/* WiFi（无默认，未配时为空串；WIFI_ENTRIES[0] 为必需，1/2 为可选备用） */
WifiEntry WIFI_ENTRIES[WIFI_ENTRIES_MAX];

/* 票务服务端完整 URL
 * TICKET_URL 为主（有默认）；TICKET_URL_1 为灾备（无默认，留空） */
char TICKET_URL[128];
char TICKET_URL_1[128];

/* IMAP（无默认，未配时为空串；端口给个常用默认值） */
char IMAP_USER[65];
char IMAP_PASS[65];
char IMAP_HOST[65];
uint16_t IMAP_PORT;

/* 唤醒时间（无默认，未配时为 255） */
uint8_t WAKE_HOUR;
uint8_t WAKE_MINUTE;

/* AES 共享密钥（无默认，首次配对前为空串） */
char AES_KEY[33];

/* "未配"哨兵值：configIsComplete 用 255 区分"未配"和合法的 0 */
#define WAKE_UNSET  255

/**
 * 把所有可配项重置为出厂默认值。
 * 只有 TICKET_URL 给出实际值；其余置为"未配"状态。
 */
void configLoadDefaults() {
  /* WiFi - 全部留空触发配网 */
  for (int i = 0; i < WIFI_ENTRIES_MAX; i++) {
    WIFI_ENTRIES[i].ssid[0] = '\0';
    WIFI_ENTRIES[i].pass[0] = '\0';
  }

  /* 票务服务端 - 默认留空触发配网；如需出厂默认值，
   * 可在 platformio.ini 通过 build_flags 注入编译期宏：
   *   -DTICKET_URL_DEFAULT="https://your-host/your-path"
   * 然后在此处用 strncpy 写入 TICKET_URL（避免把 URL 写死在源码）*/
  TICKET_URL[0] = '\0';
  TICKET_URL_1[0] = '\0';
  /* IMAP - 留空触发配网（端口给个常见值方便用户填表） */
  IMAP_USER[0] = '\0';
  IMAP_PASS[0] = '\0';
  IMAP_HOST[0] = '\0';
  IMAP_PORT = 993;

  /* 唤醒时间 - 留为"未配"哨兵值 */
  WAKE_HOUR = WAKE_UNSET;
  WAKE_MINUTE = WAKE_UNSET;

  /* AES 共享密钥 - 无默认，强制配对 */
  AES_KEY[0] = '\0';
}

/**
 * 必要字段完整性检查
 * @return true 全部齐，false 有缺失
 */
bool configIsComplete() {
  return WIFI_ENTRIES[0].ssid[0] != '\0'
      && WIFI_ENTRIES[0].pass[0] != '\0'
      && IMAP_USER[0] != '\0'
      && IMAP_PASS[0] != '\0'
      && IMAP_HOST[0] != '\0'
      && WAKE_HOUR <= 23
      && WAKE_MINUTE <= 59
      && strlen(AES_KEY) == 32;
}
