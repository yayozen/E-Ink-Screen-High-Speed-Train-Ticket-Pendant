/**
 * AES-128-CBC 对称加解密工具
 *
 * 与固件端 firmware/src/utils.cpp 完全配对：
 *   - 同一 16 字节密钥（32 字符 hex 编码）
 *   - 同一 PKCS#7 填充（Node.js crypto 默认行为）
 *   - 同一传输格式：base64( IV(16字节) + 密文 )
 *
 * 双向流程：
 *   - 后端解密：HTTP 收到 base64 串 -> 拆 IV/密文 -> AES-128-CBC 解密
 *   - 后端加密（备用）：明文 -> PKCS#7 填充 -> AES-128-CBC 加密 -> 拼接 IV -> base64
 *
 * 密钥来源（v4 协议）：
 *   - 不再使用模块级默认密钥
 *   - 调用方按 deviceId 从 config.aesKeys 查 hex 字符串，传入 hexKey 参数
 *   - 内部 hex → 16 字节 Buffer
 */
const crypto = require('crypto');

/**
 * 32 字符 hex 字符串 → 16 字节 Buffer
 * @param {string} hex 32 字符 hex 编码
 * @returns {Buffer|null} 16 字节 Buffer，非法时返回 null
 */
function hexToBuffer(hex) {
  if (typeof hex !== 'string' || !/^[0-9a-fA-F]{32}$/.test(hex)) {
    return null;
  }
  return Buffer.from(hex, 'hex');
}

/**
 * 内部：按 hex 字符串取出 16 字节 Buffer
 * @param {string} hexKey
 * @returns {Buffer} 16 字节 Buffer；非法时抛错
 */
function requireKey(hexKey) {
  const buf = hexToBuffer(hexKey);
  if (!buf) {
    throw new Error(`requireKey: 密钥非法（需要 32 字符 hex 字符串），实际: ${String(hexKey).slice(0, 8)}...`);
  }
  return buf;
}

/**
 * 解密：base64( IV(16) + AES128-CBC 密文 ) -> 原始明文
 * @param {string} base64Str 前端/固件端传来的 base64 字符串
 * @param {string} hexKey   32 字符 hex 编码的 16 字节密钥
 * @returns {string} 解密后的明文
 * @throws 输入非法 / 密钥格式错 时抛错
 */
function decrypt(base64Str, hexKey) {
  if (!base64Str || typeof base64Str !== 'string') {
    throw new Error('decrypt: 输入必须是非空字符串');
  }
  const key = requireKey(hexKey);
  const buf = Buffer.from(base64Str, 'base64');
  // 前 16 字节 IV，剩余为密文
  const iv = buf.subarray(0, 16);
  const cipherText = buf.subarray(16);

  if (cipherText.length === 0 || cipherText.length % 16 !== 0) {
    throw new Error('decrypt: 密文长度必须为 16 字节倍数');
  }

  const decipher = crypto.createDecipheriv('aes-128-cbc', key, iv);
  let plain = decipher.update(cipherText);
  plain = Buffer.concat([plain, decipher.final()]);
  return plain.toString('utf8');
}

/**
 * 加密：明文 -> base64( IV(16) + AES128-CBC 密文 )
 * @param {string} plainText 明文字符串
 * @param {string} hexKey    32 字符 hex 编码的 16 字节密钥
 * @returns {string} base64 编码的密文
 */
function encrypt(plainText, hexKey) {
  const key = requireKey(hexKey);
  const iv = crypto.randomBytes(16);
  const cipher = crypto.createCipheriv('aes-128-cbc', key, iv);
  const encrypted = Buffer.concat([
    cipher.update(plainText, 'utf8'),
    cipher.final(),
  ]);
  return Buffer.concat([iv, encrypted]).toString('base64');
}

/**
 * 加密二进制 Buffer：原始 Buffer -> base64( IV(16) + AES128-CBC 密文 )
 * @param {Buffer} buffer 原始二进制数据
 * @param {string} hexKey 32 字符 hex 编码的 16 字节密钥
 * @returns {string} base64 编码的密文
 */
function encryptBuffer(buffer, hexKey) {
  const key = requireKey(hexKey);
  const iv = crypto.randomBytes(16);
  const cipher = crypto.createCipheriv('aes-128-cbc', key, iv);
  const encrypted = Buffer.concat([
    cipher.update(buffer),
    cipher.final(),
  ]);
  return Buffer.concat([iv, encrypted]).toString('base64');
}

/**
 * 计算 CRC16 校验和
 * @param {Buffer} buffer 输入二进制数据
 * @returns {number} CRC16 校验和值
 */
function crc16(buffer) {
  let crc = 0xFFFF;
  for (let i = 0; i < buffer.length; i++) {
    crc = (crc ^ buffer[i]) & 0xFF;
    crc = (crc << 8) ^ (crc >> 8);
    crc = (crc ^ 0x1021) & 0xFFFF;
  }
  return crc;
}

module.exports = { decrypt, encrypt, encryptBuffer, hexToBuffer, crc16 };
