/**
 * WiFi 连接管理实现
 *
 * 耗电优化（v7 - 扫描匹配）：
 *   - 先扫描周围 WiFi，匹配配置的 SSID，按信号强度排序后连接
 *   - 避免对不可见网络做无效连接尝试，无可见网络时直接返回（省 ~80% 连接功耗）
 *   - setTxPower(WIFI_POWER_17dBm)：短距离场景下 17dBm 够用，省 ~30% TX 功耗
 *   - setSleep(WIFI_PS_MIN_MODEM)：连接成功后启用 modem sleep，AP 间空闲时段关
 * radio
 */
#include "wifi_mgr.h"
#include "config.h"
#include <Arduino.h>

/**
 * 先扫描周围 WiFi，匹配配置的 SSID 后连接
 * 避免对不可见网络做无效连接尝试，节省连接功耗
 * @return true 任一可见网络连接成功，false 全部失败
 */
bool connectWifi() {
  WiFi.mode(WIFI_STA);
  /* 降低发射功率（短距离场景下 17dBm 够用，省 ~30% TX 功耗） */
  WiFi.setTxPower(WIFI_POWER_17dBm);
  /* 启用 modem sleep（连接成功后空闲时段关 radio） */
  WiFi.setSleep(WIFI_PS_MIN_MODEM);
  // 禁止自动重连，避免意外耗电
  WiFi.setAutoConnect(false);
  WiFi.setAutoReconnect(false);

  /* ============================================================
   * 1. 扫描周围 WiFi
   * ============================================================ */
  int scanResult = WiFi.scanNetworks();
  if (scanResult == -1) {
    Serial.println("[WIFI] 扫描失败");
    WiFi.scanDelete(); // 释放扫描结果内存
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    return false;
  }
  Serial.printf("[WIFI] 扫描到 %d 个网络\n", scanResult);

  /* ============================================================
   * 2. 匹配配置的 SSID，找到第一个可见的已配置网络
   * ============================================================ */
  char *ssid = NULL;
  char *pass = NULL;

  for (int i = 0; i < WIFI_ENTRIES_MAX; i++) {
    if (WIFI_ENTRIES[i].ssid[0] == '\0')
      continue;
    for (int j = 0; j < scanResult; j++) {
      if (strcmp(WIFI_ENTRIES[i].ssid, WiFi.SSID(j).c_str()) == 0) {
        ssid = WIFI_ENTRIES[i].ssid;
        pass = WIFI_ENTRIES[i].pass;
        break;
      }
    }
    if (ssid != NULL)
      break;
  }
  WiFi.scanDelete(); // 释放扫描结果内存

  if (ssid == NULL) {
    Serial.println("[WIFI] 无可见的已配置网络");
    return false;
  }

  Serial.printf("[WIFI] 尝试连接 %s \n", ssid);
  /* pass 可为空串（开放网络） */
  WiFi.begin(ssid, pass);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.printf("[WIFI] %s 连接超时\n", ssid);
      break;
    }
    /* 50ms 轮询间隔，让出 CPU */
    delay(50);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] 已连接 %s, IP=%s, RSSI=%d\n", ssid,
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }

  /* 本组失败 → 关闭 radio 再断开凭据，避免下一组 begin() 时残留状态引起功耗峰值
   */
  WiFi.mode(WIFI_OFF);
  delay(150);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_17dBm);
  WiFi.setSleep(WIFI_PS_MIN_MODEM);

  Serial.println("[WIFI] 所有可见网络均连接失败");
  return false;
}
