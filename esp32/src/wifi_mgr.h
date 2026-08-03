/**
 * WiFi 连接管理
 * 仅提供 connectWifi() 工具函数
 *
 * 时间同步走服务端响应里的 ts 字段（main.cpp → setRTCTime），
 * 不再使用本地 NTP，故移除 syncNTPTime 接口
 */
#pragma once

#include <Arduino.h>
#include <WiFi.h>

/**
 * 连接 WiFi，失败返回 false
 * 内部已设 setTxPower(17dBm) + setSleep(MIN_MODEM)，调用方无需重复设置
 * @return true 连接成功，false 超时
 */
bool connectWifi();
