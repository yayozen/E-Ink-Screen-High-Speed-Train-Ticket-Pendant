/**
 * AES-128-CBC 加解密实现 —— 配对 server/src/utils.js 使用
 *
 * 加解密双方约定：
 *   - 密钥：32 字符 hex 编码的 16 字节（运行时由调用方传入，不在编译期固定）
 *   - 模式：AES-128-CBC（PKCS#7 填充）
 *   - 传输格式：base64( IV(16字节) + 密文 )
 *   - 编码：密文二进制 → 拼接 IV → base64
 *
 * 仅支持 ESP32 Arduino 内置的 mbedtls AES 上下文。
 *
 * 单包明文上限 128 字节，PKCS#7 填充后最多 144 字节，足够 IMAP 配置/车票摘要。
 */
#include "utils.h"
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <esp_random.h>
#include <stdlib.h>
#include <string.h>

/* AES_KEY 的实现在 config_defaults.cpp（与 WIFI_ENTRIES / IMAP_USER 等并列），
   本文件只引用，不再单独定义，避免 multiple definition 链接错误 */

/**
 * 把单个 hex 字符转换为 0~15；非 hex 返回 0xFF
 */
static inline uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
  if (c >= 'a' && c <= 'f') return (uint8_t)(10 + c - 'a');
  if (c >= 'A' && c <= 'F') return (uint8_t)(10 + c - 'A');
  return 0xFF;
}

/**
 * 32 字符 hex → 16 字节 binary
 */
bool hexToBytes(const String& hex, uint8_t* out, size_t outSize) {
  if ((size_t)hex.length() != outSize * 2) return false;
  for (size_t i = 0; i < outSize; i++) {
    uint8_t hi = hexNibble(hex.charAt(i * 2));
    uint8_t lo = hexNibble(hex.charAt(i * 2 + 1));
    if (hi == 0xFF || lo == 0xFF) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

/**
 * PKCS#7 填充：明文末尾补 N 个字节，每个字节的值都是 N（1 ≤ N ≤ 16）
 * 即使明文长度恰好是 16 的倍数，也要补满 16 字节的 0x10，便于接收方无歧义去填充。
 * @param in     输入明文
 * @param inLen  明文长度
 * @param out    输出缓冲（容量 ≥ inLen + 16）
 * @return 填充后的总长度
 */
static uint16_t pkcs7Pad(const uint8_t *in, uint16_t inLen, uint8_t *out) {
  uint8_t padByte = 16 - (uint8_t)(inLen % 16);   // 1..16
  memcpy(out, in, inLen);
  memset(out + inLen, padByte, padByte);
  return (uint16_t)(inLen + padByte);
}

/**
 * PKCS#7 去填充：检查末尾字节并剥离填充段
 * @param in   输入缓冲
 * @param inLen 输入长度（16 字节倍数）
 * @return 去除填充后的明文长度；填充非法时返回 inLen（保守保留原文）
 */
static uint16_t pkcs7Unpad(const uint8_t *in, uint16_t inLen) {
  if (inLen == 0) return 0;
  uint8_t padByte = in[inLen - 1];
  if (padByte == 0 || padByte > 16) return inLen;   // 非法填充
  // 校验填充段是否全部一致
  for (uint8_t i = 1; i <= padByte; i++) {
    if (in[inLen - i] != padByte) return inLen;
  }
  return (uint16_t)(inLen - padByte);
}

/**
 * AES-128-CBC 加/解密底层封装（按 mode 切换方向）
 * @param key  16 字节密钥
 * @param iv   16 字节初始向量（CBC 模式下内部会被原地更新）
 * @param in   输入数据缓冲（长度须为 16 的倍数）
 * @param len  数据长度
 * @param out  输出缓冲（容量 ≥ len）
 * @param mode MBEDTLS_AES_ENCRYPT 或 MBEDTLS_AES_DECRYPT
 */
static void aesCbcCrypt(const uint8_t *key, uint8_t *iv,
                        const uint8_t *in, uint16_t len,
                        uint8_t *out, int mode) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  if (mode == MBEDTLS_AES_ENCRYPT) {
    mbedtls_aes_setkey_enc(&aes, key, 128);
  } else {
    mbedtls_aes_setkey_dec(&aes, key, 128);
  }
  mbedtls_aes_crypt_cbc(&aes, mode, len, iv, in, out);
  mbedtls_aes_free(&aes);
}

/**
 * 加密：明文 -> base64( IV(16) + AES128-CBC(PKCS#7 填充后) )
 * IV 由 esp_random() 硬件真随机生成，每次调用都不同。
 */
String aesEncryptBase64(const String& plain, const String& hexKey) {
  const size_t rLen = plain.length();
  if (rLen == 0 || rLen > 128) {
    return String();
  }
  uint8_t key[16];
  if (!hexToBytes(hexKey, key, 16)) {
    return String();
  }

  // 1) 生成 16 字节随机 IV
  uint8_t iv[16];
  for (int i = 0; i < 16; i++) {
    iv[i] = (uint8_t)(esp_random() & 0xFF);
  }

  // 2) PKCS#7 填充
  //    关键：PKCS#7 规则下，inLen 已经是 16 字节倍数时也要再补满 1 个块
  //    (全部填 0x10)，所以 pLen 最大是 rLen + 16。
  //    之前用 ((rLen+15)/16)*16 算 paddedLen，当 rLen=16 时只分配 16 字节，
  //    但 pkcs7Pad 实际写入 32 字节 → memset 越界写 16 字节破坏堆，
  //    后续 aesCbcCrypt 越界读触发 StoreProhibited panic。
  //    修复：永远多预留 1 个块（16 字节）用于必填充。
  const size_t paddedLen = ((rLen / 16) + 1) * 16;
  uint8_t *rawBuf = (uint8_t *)malloc(rLen);
  uint8_t *padBuf = (uint8_t *)malloc(paddedLen);
  uint8_t *cipher = (uint8_t *)malloc(paddedLen);
  if (!rawBuf || !padBuf || !cipher) {
    free(rawBuf);
    free(padBuf);
    free(cipher);
    return String();
  }

  memcpy(rawBuf, plain.c_str(), rLen);
  const uint16_t pLen = pkcs7Pad(rawBuf, (uint16_t)rLen, padBuf);

  // 3) AES-128-CBC 加密
  //    注意：mbedtls_aes_crypt_cbc() 会把 iv 原地更新为"最后一块密文"，
  //    所以必须在调用前先备份 IV；否则 enstr 的前 16 字节会被错误地替换成
  //    最后一块密文，导致服务端解密的第一个 AES 块错乱。
  uint8_t ivBackup[16];
  memcpy(ivBackup, iv, 16);
  aesCbcCrypt(key, iv, padBuf, pLen, cipher, MBEDTLS_AES_ENCRYPT);

  // 4) 拼接 IV + 密文，base64 编码
  //    这里必须使用调用 aesCbcCrypt 之前备份好的原始 IV，保证服务端能正确拆出 IV/密文。
  const size_t combinedLen = 16 + pLen;
  uint8_t *combined = (uint8_t *)malloc(combinedLen);
  const size_t b64BufLen = ((combinedLen + 2) / 3) * 4 + 1;
  uint8_t *b64Buf = (uint8_t *)malloc(b64BufLen);
  if (!combined || !b64Buf) {
    free(rawBuf);
    free(padBuf);
    free(cipher);
    free(combined);
    free(b64Buf);
    return String();
  }

  memcpy(combined, ivBackup, 16);
  memcpy(combined + 16, cipher, pLen);
  size_t encLen = 0;
  if (mbedtls_base64_encode(b64Buf, b64BufLen, &encLen,
                            combined, combinedLen) != 0) {
    free(rawBuf);
    free(padBuf);
    free(cipher);
    free(combined);
    free(b64Buf);
    return String();
  }
  if (encLen >= b64BufLen) {
    free(rawBuf);
    free(padBuf);
    free(cipher);
    free(combined);
    free(b64Buf);
    return String();
  }
  b64Buf[encLen] = '\0';

  String result((const char *)b64Buf);
  free(rawBuf);
  free(padBuf);
  free(cipher);
  free(combined);
  free(b64Buf);
  return result;
}

/**
 * 解密：base64( IV(16) + AES128-CBC 密文 ) -> 明文
 * 失败（base64 非法、长度不对齐 16 字节）时返回空串。
 */
String aesDecryptBase64(const String& base64Str, const String& hexKey) {
  const size_t inLen = base64Str.length();
  if (inLen == 0) return String();
  uint8_t key[16];
  if (!hexToBytes(hexKey, key, 16)) {
    return String();
  }

  // 1) base64 解码：先估算解码后最大长度，准备输出缓冲
  //    base64 库 1.4.0 的 decode_base64 是 C 风格函数：返回解码后的实际字节数
  //    单段输出：输入 + 输出缓冲，返回总字节数；前 16 字节为 IV，剩余为密文
  const size_t outBufLen = inLen + 16;
  uint8_t *outBuf = (uint8_t *)malloc(outBufLen);
  size_t total = 0;
  if (!outBuf) return String();
  if (mbedtls_base64_decode(outBuf, outBufLen, &total,
                            (const unsigned char *)base64Str.c_str(), inLen) != 0) {
    free(outBuf);
    return String();
  }
  if (total < 16) {
    free(outBuf);
    return String();   // 解码失败或长度不足
  }
  const size_t pLen = total - 16;
  if (pLen == 0 || pLen % 16 != 0) {
    free(outBuf);
    return String();   // 密文长度须为 16 字节倍数
  }

  // 2) AES-128-CBC 解密：前 16 字节为 IV，剩余为密文
  uint8_t iv[16];
  memcpy(iv, outBuf, 16);
  uint8_t *plain = (uint8_t *)malloc(pLen);
  if (!plain) {
    free(outBuf);
    return String();
  }
  aesCbcCrypt(key, iv, outBuf + 16, (uint16_t)pLen, plain, MBEDTLS_AES_DECRYPT);

  // 3) 去除 PKCS#7 填充
  const uint16_t realLen = pkcs7Unpad(plain, (uint16_t)pLen);
  String result;
  result.reserve(realLen);
  for (uint16_t i = 0; i < realLen; ++i) {
    result += (char)plain[i];
  }
  free(outBuf);
  free(plain);
  return result;
}


/**
 * 通过时间戳设置 RTC 时间
 *
 * 时间戳非法（0 或负数）时直接 return，不更新 RTC：
 *   - 服务端 JSON 解析失败时 ts=0，旧行为会写入 2023-03-28 这个错误时间，
 *     导致 secondsUntilNextWake 算出错误的 24h 兜底
 *   - 新行为保留 RTC 原值（上次同步的成功时间），即使 RTC 也未初始化，
 *     deepSleepUntil 内部仍有 24h 兜底，不会卡死
 *
 * 时区设置每次都执行（开销极小，~5ms）；
 * 若后续要优化为"只设一次"，可用 RTC_NOINIT_ATTR 守卫，
 * 但需要 ESP32-C3 RTC 内存可用性验证，暂不做。
 *
 * @param timestamp 以秒为单位的时间戳（UTC 时间）
 */
void setRTCTime(time_t timestamp) {
  if (!timestamp || timestamp <= 0) {
    Serial.println("[RTC] 时间戳非法，跳过 RTC 更新");
    return;
  }
  // 写入系统RTC（设置的是UTC基准时间）
  struct timeval tv = {0};
  tv.tv_sec = timestamp;
  tv.tv_usec = 0;
  settimeofday(&tv, NULL);
  // 配置东八区时区，保证getLocalTime返回北京时间
  setenv("TZ", "CST-8", 1);
  // 关键：调用 tzset() 使时区设置生效
  // 缺少此调用会导致 mktime() 转换时间戳时使用错误的时区偏移量，
  // 进而导致 deep sleep 唤醒时间计算不准确
  tzset();
}


/**
 * 打印当前本地时间（从 NTP 同步的 RTC）
 * 串口打印当前时间（调试用）
 */
void printLocalTime() {
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo)) {
    Serial.println("获取时间失败");
    return;
  }
  Serial.printf("[WIFI] 当前时间：%04d-%02d-%02d %02d:%02d:%02d",
                timeInfo.tm_year + 1900,
                timeInfo.tm_mon + 1,
                timeInfo.tm_mday,
                timeInfo.tm_hour,
                timeInfo.tm_min,
                timeInfo.tm_sec);
  Serial.println();
}
