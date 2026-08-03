/**
 * BLE 配网：基于 NimBLE-Arduino
 *
 * 工作流：
 *   setup() 启动时 → 调用 bleCfgEnter(waitConnMs)
 *                  → ESP32 起 NimBLE peripheral + 配网广播
 *                  → 浏览器（Web Bluetooth）连接 → 读 HTML characteristic 拿到配网页面
 *                  → 填表后 writeValueWithResponse 分包发送 JSON
 *                  → ESP32 累积到 '\n' → 解析 → configManagerApplyJson 落盘
 *                  → 写入成功响应 → 客户端 disconnect → 退出配网模式
 *
 * 两阶段超时（waitConnMs > 0 时启用）：
 *   - 阶段 1：waitConnMs 内若无人连接 → 返回 NO_CONNECT，调用方继续正常流程
 *   - 阶段 2：一旦客户端连接，启动 CFG_BLE_TIMEOUT_MS（5 分钟）会话超时
 *
 * 设计要点：
 *   - 写 characteristic 用 write-without-response（流式分片 20B 累积）
 *   - 配网模式独占：会先关 WiFi，结束/超时后由调用方决定 deep sleep / restart
 *   - 连接后会话超时默认 5 分钟
 */
#pragma once

#include <Arduino.h>

/**
 * 配网模式退出原因——区分超时和真配网完成，否则会把"已配好"误判成超时
 * 让设备睡了 24h，用户体验严重割裂（明明刚配完却要等一天）
 */
enum CfgResult {
  CFG_RESULT_TIMEOUT    = 0,  // 会话超时（waitConnMs=0 时表示整体 5 分钟超时；waitConnMs>0 时表示连接后 5 分钟超时）
  CFG_RESULT_CANCELED   = 1,  // 连过但没保存就断开
  CFG_RESULT_COMPLETED  = 2,  // set / reset 成功落盘
  CFG_RESULT_NO_CONNECT = 3,  // 等待窗口内无人连接（仅 waitConnMs > 0 时可能出现）
  CFG_RESULT_REFRESH    = 4,  // 用户点了"刷新车票"→退出配网，走 main.cpp 抓票流程（带强刷标识）
};

/**
 * 启动配网模式（阻塞，直到连接断开/超时/配置成功）
 * 进入前会关闭 WiFi + 关闭其他外设
 * @param waitConnMs 等待客户端连接的窗口时长（毫秒）：
 *                   - 0   ：沿用旧逻辑，整体 CFG_BLE_TIMEOUT_MS（5 分钟）超时
 *                   - >0  ：先等 waitConnMs 时间；窗口内无人连接返回 NO_CONNECT；
 *                           有人连接则启动 5 分钟会话超时
 * @return 退出原因，调用方根据结果选择 esp_restart（COMPLETED）
 *         或 deepSleepUntil（TIMEOUT / CANCELED）或继续抓票（NO_CONNECT）
 */
CfgResult bleCfgEnter(uint32_t waitConnMs = 0);
