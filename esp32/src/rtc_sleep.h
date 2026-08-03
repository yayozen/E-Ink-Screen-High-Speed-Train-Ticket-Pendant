/**
 * RTC 定时 deep sleep 唤醒
 * 使用 esp_sleep_enable_timer_wakeup 设置唤醒时间
 */
#pragma once

#include <Arduino.h>
#include <time.h>

/**
 * 计算距离目标时刻的微秒数（已处理 0~24h 跨天）
 * @param targetHour 0-23
 * @param targetMinute 0-59
 * @return 距离目标时刻的秒数，若 <60s 则取整 60s
 */
uint64_t secondsUntilNextWake(int targetHour, int targetMinute);

/**
 * 进入 deep sleep 并在指定时间唤醒
 * @param targetHour 唤醒时刻 - 小时
 * @param targetMinute 唤醒时刻 - 分钟
 */
void deepSleepUntil(int targetHour, int targetMinute);

/**
 * 进入 deep sleep 指定秒数后唤醒（用于失败退避 / 401 快重试等场景）
 * 内部已调用 prepareDeepSleep() 关闭外设 + 电源域，调用方无需重复关 WiFi/Serial
 * @param seconds 休眠秒数（必须 > 0）
 */
void deepSleepFor(uint32_t seconds);

/**
 * 获取当前 RTC 时间结构
 */
bool getRtcTime(struct tm *out);

/**
 * 闪烁 LED 并倒计时
 */
void blinkLedAndSleepCountdown(int targetHour, int targetMinute);
