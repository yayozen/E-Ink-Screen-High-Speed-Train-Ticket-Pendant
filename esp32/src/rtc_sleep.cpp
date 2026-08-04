/**
 * RTC 定时唤醒实现
 */
#include "rtc_sleep.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <time.h>
#include <SPI.h>

#define BLINK_INTERVAL 15  // 闪烁间隔 15s
#define BLINK_TIMES 50 // 闪烁总时长 50ms
// RTC 内存：掉电不丢失，存业务倒计时和LED状态
RTC_DATA_ATTR int nextCountdown = 0;

/**
 * 获取 RTC 当前时间
 */
bool getRtcTime(struct tm *out) {
  return getLocalTime(out, 0);
}

/**
 * 计算距离下次目标时刻的秒数
 * 严格保证返回值 >= 60 秒，避免 0 秒导致立即唤醒
 * 未配（hour=255 或 minute=255）/ RTC 无时间 → 统一兜底 24 小时
 */
uint64_t secondsUntilNextWake(int targetHour, int targetMinute) {
  /* 合法性检查：缺配置时 WAKE_HOUR/WAKE_MINUTE=255，若直接喂给 mktime
     会让 tm_hour=255 引发未定义行为；这里统一兜底 24 小时，
     让设备有节奏地醒来等用户配网（用户也可长按 BOOT 强制再配） */
  if (targetHour < 0 || targetHour > 23 || targetMinute < 0 || targetMinute > 59) {
    Serial.printf("[SLEEP] 唤醒时间非法 (%d:%d)，兜底 24 小时\n", targetHour, targetMinute);
    return 24ULL * 3600;
  }

  struct tm now;
  if (!getRtcTime(&now)) {
    // 拿不到时间，保守按 24 小时后唤醒
    return 24ULL * 3600;
  }

  // 构造今日的目标唤醒时间点
  struct tm target = now;
  target.tm_hour = targetHour;
  target.tm_min = targetMinute;
  target.tm_sec = 0;
  // 中国不实行夏令时，显式指定 tm_isdst = 0 避免 mktime() 行为不确定
  target.tm_isdst = 0;

  // 转为时间戳比较
  // 注意：mktime() 会修改传入的 struct tm（规范化 tm_wday/tm_yday/tm_isdst），
  // 但 target 是 now 的副本，互不影响
  time_t t_now = mktime(&now);
  time_t t_target = mktime(&target);
  if (t_target <= t_now) {
    // 已过当日目标时刻，改为次日
    t_target += 24 * 3600;
  }
  uint64_t diff = (uint64_t)(t_target - t_now);
  Serial.printf("[SLEEP] 当前时间戳=%u, 目标时间戳=%u, 相差=%llu 秒\n",
                (unsigned)t_now, (unsigned)t_target, (unsigned long long)diff);
  // 容差10分钟直接返回差值24小时
  if (diff <= 10 * 60ULL) {
    diff = 24ULL * 3600 + diff;
  }
  return diff;
}

/**
 * 进入 deep sleep 前关闭所有非必要外设，降低 sleep 电流
 *
 * 关闭项：
 *   - WiFi radio（外层调用方一般已关，这里幂等再关一次防漏）
 *   - UART（Serial.end）：UART 外设在 deep sleep 期间默认保留电源，可省 ~1mA
 *   - 墨水屏控制引脚 CS/DC/RST/BUSY：设为 INPUT_PULLDOWN 防止浮空漏电
 *   - 板载 LED：设为 INPUT 避免反向点亮（C3 的 GPIO8 默认高电平会亮）
 *
 * 电源域策略（显式 OFF，避免 ESP_PD_OPTION_AUTO 在某些场景下保留 RTC_PERIPH）：
 *   - RTC_PERIPH：OFF（不使用 ULP / 触摸唤醒）
 *   - XTAL：OFF（不使用外部晶振）
 *   注：RTC_SLOW_MEM 默认 AUTO 保留，存放 deep sleep 标志位所需
 */
static void prepareDeepSleep() {
  // 清空全部历史唤醒源，杜绝残留唤醒源冲突导致定时失效
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  /* 关闭 WiFi radio（幂等：已关再关一次不报错） */
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  delay(10);

   // 释放硬件SPI总线，释放外设锁，避免干扰RTC时钟
  SPI.end();

  /* 关闭墨水屏控制引脚（设为下拉避免浮空漏电） */
  pinMode(EPD_CS, INPUT_PULLDOWN);
  pinMode(EPD_DC, INPUT_PULLDOWN);
  pinMode(EPD_RST, INPUT_PULLDOWN);
  pinMode(EPD_BUSY, INPUT_PULLDOWN);
  /* SPI 时钟/数据线也下拉（避免浮空引起 IO 漏电） */
  pinMode(EPD_SCK, INPUT_PULLDOWN);
  pinMode(EPD_MOSI, INPUT_PULLDOWN);

  /* 关闭板载 LED（部分板子 LED 默认高电平点亮，浪费电） */
  pinMode(LED_BUILTIN, INPUT);

  /* 显式关闭非必要电源域 */
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
  esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_OFF);

  /* 关闭 UART：生产环境不需要日志，可省 ~1mA sleep 电流 */
  Serial.flush();
  Serial.end();
}

/**
 * 进入 deep sleep 并按目标时间唤醒
 */
void deepSleepUntil(int targetHour, int targetMinute) {
  uint64_t secs = secondsUntilNextWake(targetHour, targetMinute);
  uint64_t hours = secs / 3600;
  uint64_t minutes = (secs % 3600) / 60;
  Serial.printf("[SLEEP] 下次执行: %02d:%02d，约 %llu 小时 %llu 分后\n",
                targetHour, targetMinute, (unsigned long long)hours, (unsigned long long)minutes);
  // 计算倒计时轮数
  // 每个轮数闪烁一次，共倒计时轮数轮
  nextCountdown = (int)(secs / (BLINK_INTERVAL + BLINK_TIMES / 1000.0));

  prepareDeepSleep();
  esp_sleep_enable_timer_wakeup((uint64_t)BLINK_INTERVAL * 1000000ULL);
  esp_deep_sleep_start();
}

/**
 * 进入 deep sleep 指定秒数后唤醒（失败退避 / 401 快重试用）
 * 与 deepSleepUntil 共用 prepareDeepSleep()，保证 sleep 电流一致
 */
void deepSleepFor(uint32_t seconds) {
  if (seconds == 0) seconds = 1;
  Serial.printf("[SLEEP] 休眠 %u 秒\n", (unsigned)seconds);
  prepareDeepSleep();
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}

/**
 * 进入 deep sleep 且不设置任何唤醒源
 * prepareDeepSleep() 已 esp_sleep_disable_wakeup_source(ALL)，这里不再 enable 任何源，
 * 直接 esp_deep_sleep_start() 即可永久睡眠，仅 RST / 重新上电可唤醒。
 * 用于缺配置场景：24h 定时唤醒无意义（依旧没配置），等用户主动重启配网。
 */
void deepSleepNoWakeup() {
  Serial.println("[SLEEP] 进入永久 deep sleep（仅 RST/重新上电可唤醒）");
  prepareDeepSleep();
  esp_deep_sleep_start();
}

/**
 * 闪烁 LED 并倒计时
 */
void blinkLedAndSleepCountdown(int targetHour, int targetMinute) {

  Serial.printf("[SLEEP] 倒计时 %d 轮，每轮 %d 秒\n", nextCountdown, BLINK_INTERVAL);

  pinMode(LED_BUILTIN, OUTPUT);
  
  
  #if defined(IS_XIAO_ESP32S3)
    // 开灯
    digitalWrite(LED_BUILTIN, LOW);
    // 让 LED 状态可见
    delay(BLINK_TIMES);
    // 关灯
    digitalWrite(LED_BUILTIN, HIGH);
  #else
    // 开灯
    digitalWrite(LED_BUILTIN, HIGH);
    // 让 LED 状态可见
    delay(BLINK_TIMES);
    // 关灯
    digitalWrite(LED_BUILTIN, LOW);
  #endif

  nextCountdown--;

  if (nextCountdown > 1) {
    deepSleepFor(BLINK_INTERVAL);
  }
  
  // 这里到达目标时间，没有被睡眠阻止，唤醒主程序走后序逻辑
  Serial.printf("[SLEEP] 倒计时结束，唤醒主程序\n");
}
