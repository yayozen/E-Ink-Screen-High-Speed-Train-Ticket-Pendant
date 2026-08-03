/**
 * AES-128-CBC 对称加解密工具
 *
 * 用途：固件端（ESP32-D0WDQ6（V3 版本））与后端（Node.js）通过 16 字节共享密钥通信，
 *       密钥由用户在 BLE 配网页面生成后通过 POST /key 推送到服务端（双方持久化到本地），
 *       抓票时不再带密钥。
 *
 *       服务端对应文件：server/src/utils.js
 *
 * 整体流程（两方向对称）：
 *   加密：明文 -> PKCS#7 填充 -> AES-128-CBC 加密 -> 拼接 IV(16) + 密文 -> base64
 *   解密：base64 -> 拆分 IV(16) + 密文 -> AES-128-CBC 解密 -> 去除 PKCS#7 填充 -> 明文
 *
 * 注意：
 *   - 密钥来源：NVS 中读出的 hex 字符串（AES_KEY[33]，32 hex chars + '\0'）
 *   - 首次配对前 NVS 无密钥 → 调用方应保证不调用加解密接口
 *   - 加密时 IV 由 esp_random() 硬件真随机生成，绝不复用
 *   - 单包明文上限 128 字节（包含填充后不超过 144 字节），足够传递 IMAP 参数等小报文
 */
#pragma once

#include <Arduino.h>
#include "config.h"   /* 引用 AES_KEY 等运行时变量声明 */

/**
 * 加密：明文字符串 -> base64( IV(16) + AES128-CBC(PKCS#7 填充后) )
 * @param plain  待加密的明文字符串（≤128 字节）
 * @param hexKey 32 字符 hex 编码的 16 字节密钥（必须来自 AES_KEY）
 * @return base64 编码的密文；hexKey 非法 / 输入超长时返回空串
 */
String aesEncryptBase64(const String& plain, const String& hexKey);

/**
 * 解密：base64( IV(16) + AES128-CBC 密文 ) -> 明文字符串
 * @param base64Str base64 编码的密文
 * @param hexKey    32 字符 hex 编码的 16 字节密钥
 * @return 解密后的明文；输入非法（base64 失败、长度不对齐 16 字节、密钥非法）时返回空串
 */
String aesDecryptBase64(const String& base64Str, const String& hexKey);

/**
 * 把 32 字符 hex 字符串转换为 16 字节 binary buffer
 * @param hex  32 字符 hex 字符串
 * @param out  至少 16 字节的输出 buffer
 * @return true 转换成功，false 长度或字符非法
 */
bool hexToBytes(const String& hex, uint8_t* out, size_t outSize);

/**
 * 通过时间戳设置 RTC 时间
 * @param timestamp 以秒为单位的时间戳（UTC 时间）
 */
void setRTCTime(time_t timestamp);

/**
 * 打印当前本地时间（从 NTP 同步的 RTC）
 * 串口打印当前时间（调试用）
 */
void printLocalTime();
