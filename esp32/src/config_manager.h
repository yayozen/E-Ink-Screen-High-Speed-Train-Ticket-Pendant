/**
 * 配置持久化管理
 * - 把 config.h 中的可配项保存到 NVS（Preferences 抽象）
 * - IMAP_PASS 等敏感字段用 AES_KEY 加密后落盘
 * - begin() 时按"默认值 → NVS 覆盖"顺序填充到 config.h 的 extern 变量
 * - save() 接收一份 JsonDocument（来自 BLE 配网），按字段名落盘
 */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

/**
 * 启动配置管理：先填默认值，再用 NVS 中存在的项覆盖
 * 必须在所有用到 extern 配置的模块初始化之前调用一次
 */
void configManagerBegin();

/**
 * 清空 NVS 中所有可配项（恢复出厂），并把内存变量重置为默认值
 * 一般通过 BLE 配网页面的"恢复出厂"按钮触发
 */
void configManagerResetToDefaults();

/**
 * 把当前内存中的所有可配项落盘（敏感字段 AES 加密）
 * BLE 配网写入 JSON 后由 ble_cfg 调用
 */
void configManagerSaveAll();

/**
 * 用 BLE 传入的 JSON 局部更新可配项
 * @param doc 来自浏览器的配置 JSON（结构见 ble_cfg.cpp 协议注释）
 * @return 写入成功的 key 列表（用于 BLE 响应）
 */
String configManagerApplyJson(const JsonDocument &doc);

/**
 * 把当前所有可配项导出为 JSON（用于 BLE 读出 / 配网页面预填）
 * @param out 容量至少 1024 的 JsonDocument
 */
void configManagerDumpJson(JsonDocument &out);

/**
 * 查询 NVS 中"密钥已推送"标志（key_pushed）
 * 用于 main.cpp 启动时判断：已推送则跳过 POST /key，未推送则需推送
 * @return true 已成功推送到服务端，false 未推送 / 推送后被配置改动清除
 */
bool configManagerIsKeyPushed();

/**
 * 把"密钥已推送"标志置 true（推送成功后调用）
 * 下次启动时 main.cpp 据此跳过 POST /key
 */
void configManagerMarkKeyPushed();

/**
 * 把"密钥已推送"标志重置为 false
 * 场景：服务端 datas.json 误删导致 /ticket 返回 401（设备未注册），
 *       硬件端检测到后调用此函数，下次启动会重新 POST /key 推送密钥，
 *       避免陷入"kp=true 但服务端无密钥"的死锁
 */
void configManagerResetKeyPushed();

/**
 * 挂件图（idle image）持久化
 *
 * 用途：服务端无票时作为 fallback 显示的图片，由用户在 BLE 配网页面通过
 *       canvas 缩放到 250×122 + Atkinson 抖动成 1bit 后 base64 上传。
 *       固件端仅作 NVS blob 存取，不做图像处理。
 *
 * 存储位置：NVS namespace="cfg", key="pb_img"（pendant bitmap）
 * 数据格式：250×122 = 32 字节/行 × 122 行 = 3904 字节 binary
 *           1bit-per-pixel，MSB-first，行优先（与 epaperDrawBitmap 一致）
 *
 * 不加密：挂件图本身就是公开的，泄露 NVS 内容不涉及隐私；
 *        减少配对耗时、避免依赖 AES_KEY 即可显示。
 */
#include <stddef.h>
#include <stdint.h>

/**
 * 查询 NVS 是否已存有挂件图
 * @return true 已有完整 1bit 位图，false 未存或长度不匹配
 */
bool configManagerHasIdleImage();

/**
 * 把 1bit 位图写入 NVS（覆盖式）
 * @param bitmap 250×122 的 1bit 数据指针
 * @param len    数据长度（必须等于 EPD_BITMAP_SIZE）
 * @return true 写入成功，false 参数非法或 NVS 写失败
 */
bool configManagerSaveIdleImage(const uint8_t *bitmap, size_t len);

/**
 * 从 NVS 读出 1bit 位图
 * @param bitmap 输出 buffer（容量必须等于 EPD_BITMAP_SIZE）
 * @param len    期望读取长度（用于校验）
 * @return true 读出成功且长度匹配，false 失败
 */
bool configManagerLoadIdleImage(uint8_t *bitmap, size_t len);

/**
 * 从 NVS 清除挂件图
 * 恢复出厂时一并调用
 */
void configManagerClearIdleImage();
