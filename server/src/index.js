/**
 * 12306 购票邮件抓取 → 墨水屏位图服务（v5：动态 AES 密钥 + bitmap 签名去重）
 *
 * 职责：
 *   1. POST /ticket  接收硬件端 AES 加密的 IMAP 配置，渲染 1bit 位图返回
 *   2. POST /key     接收硬件端推送的共享密钥，落盘到 datas.json 的 aesKeys[deviceId]
 *
 * 密钥协议（v4）：
 *   - 设备首次配对时 POST /key { deviceId, key } 把 32 字符 hex 密钥存到 datas.json
 *   - 抓票时设备 POST /ticket（带 X-Device-Id header），服务端从 aesKeys 查密钥解密
 *   - 密钥不在抓票请求中传输，避免长期泄露
 *
 * 位图签名去重（v5）：
 *   - 渲染后对 bitmap 计算 SHA-256 签名（前 16 字符 hex），按 deviceId 持久化到 datas.json 的 bitmapSigns
 *   - 与上次签名相同则不下发 bitmap，硬件端依据两个布尔字段决定行为：
 *       needUpdate : bitmap 是否有变化（true=需解密并刷新屏幕，false=跳过刷新直接睡眠）
 *       hasTicket  : 当前是否有车票（true=有票，false=无票，用于日志或睡眠周期决策）
 *     仅 needUpdate=true 时响应才带 bitmap 字段
 *
 * 测试开关（.env）：
 *   MOCK_TICKET=true  跳过 IMAP 抓取，用 DEFAULT_EMPTY_TICKET 渲染（验证链路 / 生成 PNG）
 *   SAVE_PREVIEW=true 把 needUpdate=true 的 bitmap 存为 PNG 到 tmp-previews/（调试渲染效果）
 *
 * 流程：
 *   配对：固件端 → POST /key → 写 datas.json
 *   抓票：固件端 → POST /ticket (Header: X-Device-Id) → 查 key → 解密 enstr → 抓邮件
 *         → 渲染位图 → 计算签名 → 比对 → 加密 → 返回 → 固件端解密
 */
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const chalk = require('chalk');
const { decrypt, encryptBuffer } = require('./utils');
const { renderTicket, bitmapToPng, DEFAULT_EMPTY_TICKET, scale1bitBitmap, SCREEN_W, SCREEN_H } = require('./render-bitmap');
const {
  getDeviceKey,
  setDeviceKey,
  getDeviceBitmapSign,
  setDeviceBitmapSign,
  getDeviceImapCursor,
  setDeviceImapCursor,
  CONFIG_FILE,
  MOCK_TICKET,
  SAVE_PREVIEW,
} = require('./config');
const { getTicket } = require('./imap-fetcher');

/* ============================================================
 * 位图签名工具
 * ============================================================ */

/**
 * 计算 1bit bitmap 的 SHA-256 签名（取前 16 字符 hex）
 *   - 16 字符 hex = 64 bit，对 3904 字节位图去重足够
 *   - 比 CRC16/CRC32 抗碰撞更强，避免硬件端漏刷新
 * @param {Buffer} bitmap
 * @returns {string} 16 字符 hex 签名
 */
function computeBitmapSign(bitmap) {
  return crypto.createHash('sha256').update(bitmap).digest('hex').slice(0, 16);
}

/* ============================================================
 * HTTP 服务
 * IMAP 抓取逻辑已抽出到 ./imap-fetcher.js，便于 test-finder 复用
 * ============================================================ */

function makeReply(ok, message, data) {
  const timestamp = Math.floor(Date.now() / 1000);
  return { ok, message, ...data, ts: timestamp };
}
function makeSuccessReply(data, message = 'success') {
  return makeReply(true, message, data);
}
function makeErrorReply(message, data) {
  return makeReply(false, message, data || null);
}

/**
 * 脱敏 IMAP 明文日志：只打印 user/host/port，不打印 pass
 * @param {string} plain "user|pass|host|port|useSecure"
 * @returns {string} 脱敏后的字符串
 */
function maskImapPlain(plain) {
  const parts = plain.split('|');
  if (parts.length < 3) return '(invalid)';
  return `${parts[0]}@${parts[2]}:${parts[3] || '993'}`;
}

/** tmp-previews 目录（SAVE_PREVIEW=true 时存 PNG 用） */
const PREVIEW_DIR = path.resolve(__dirname, '..', 'tmp-previews');

/**
 * 启动 Fastify 服务
 */
function startServer() {
  const fastify = require('fastify')({ logger: true });

  // 健康检查
  fastify.get('/', async () => {
    return makeSuccessReply(null, '墨水屏位图服务运行中');
  });

  /**
   * POST /key
   * 固件端在配对时调用，把 { deviceId, key } 写入 config.aesKeys
   * 后续抓票请求会按 deviceId 查 key
   */
  fastify.post('/key', async (request, reply) => {
    const { deviceId, key } = request.body || {};
    if (!deviceId || typeof deviceId !== 'string') {
      reply.code(400);
      return makeErrorReply('bad request: deviceId is required');
    }
    if (!key || !/^[0-9a-fA-F]{32}$/.test(key)) {
      reply.code(400);
      return makeErrorReply('bad request: key must be 32 hex chars (AES-128)');
    }
    const ok = await setDeviceKey(deviceId, key);
    if (!ok) {
      reply.code(500);
      return makeErrorReply('failed to write datas.json');
    }
    console.log(chalk.green(`    ✓ 设备 ${deviceId} 已注册密钥（前 4: ${key.slice(0, 4)}****）`));
    return makeSuccessReply({ deviceId }, 'key registered');
  });

  /**
   * POST /ticket
   * 抓票请求：Header X-Device-Id 必须，按 deviceId 查 config.aesKeys 拿密钥解密 enstr
   * 渲染完成后用同密钥加密位图返回
   */
  fastify.post('/ticket', async (request, reply) => {
    const { enstr } = request.body || {};
    const deviceId = request.headers['x-device-id'];
    // 标记是否强制渲染（默认 false），不走缓存位图
    // 注意：HTTP header 值永远是字符串，必须与 '1' 比较
    const forceRender = request.headers['x-force-render'] === '1';

    if (!enstr || typeof enstr !== 'string') {
      reply.code(400);
      return makeErrorReply('bad request: enstr is required');
    }
    if (!deviceId) {
      reply.code(401);
      return makeErrorReply('missing X-Device-Id header (call POST /key first)');
    }

    /* 1) 查设备密钥 */
    const hexKey = getDeviceKey(deviceId);
    if (!hexKey) {
      reply.code(401);
      return makeErrorReply(`device '${deviceId}' not registered, call POST /key first`);
    }
    console.log(chalk.gray(`    - device=${deviceId} key=${hexKey.slice(0, 4)}****`));

    /* 2) 解密 IMAP 参数 */
    let imapCfg;
    try {
      const plain = decrypt(enstr, hexKey);
      console.log(chalk.gray(`    - 解密后: ${maskImapPlain(plain)}`));
      const parts = plain.split('|');
      if (parts.length < 3) {
        throw new Error('解密后字段不足（需 user|pass|host|port|useSecure）');
      }
      imapCfg = {
        user: parts[0],
        pass: parts[1],
        host: parts[2],
        port: parts[3] || '993',
        useSecure: parts[4] !== 'false',
      };
    } catch (e) {
      console.error(chalk.red('    ✗ 解密失败:'), e.message);
      reply.code(400);
      return makeErrorReply(`decrypt failed: ${e.message}`);
    }

    /* 3) 抓票
       MOCK_TICKET=true 时跳过 IMAP，用 DEFAULT_EMPTY_TICKET 渲染（测试用）
       正常模式 getTicket 抛错 → 返回 5xx，硬件端走 FETCH_FAILED 短周期重试
       getTicket 返回 null → 真的无票，渲染空票
       fromCache=true 表示复用了缓存车票（无新邮件 / 新邮件非车票） */
    let ticket;
    let fromCache = false;
    if (MOCK_TICKET) {
      console.log(chalk.yellow('    ! MOCK_TICKET=true，跳过 IMAP 抓取'));
      ticket = DEFAULT_EMPTY_TICKET;
    } else {
      try {
        const r = await getTicket(imapCfg, deviceId, forceRender);
        ticket = r.ticket;
        fromCache = r.fromCache;
      } catch (e) {
        console.error(chalk.red('    ✗ IMAP 抓取失败:'), e.message);
        reply.code(502);
        return makeErrorReply(`imap fetch failed: ${e.message}`);
      }
    }

    /* 3.5) 同日重复请求快速路径（仅当复用缓存车票 且 非强制刷新时启用）
       - 车票缓存未变 + 今天已经渲染过 → daysLeft 必未变 → 位图必未变
         → 跳过渲染和签名计算，直接返回 needUpdate=false
       - 覆盖场景：硬件端 HTTP 失败重试、网络抖动重复请求
       - 不覆盖场景：跨日请求（倒计时变了必须渲染）、有新车票邮件（fromCache=false）、
         强制刷新（forceRender=true，用户点了"刷新车票"按钮，必须重新渲染并下发位图） */
    if (fromCache && !forceRender) {
      /* en-CA locale 在 Node.js 中输出 YYYY-MM-DD 格式（标准 ISO 风格），
         比 'zh-CN' + replace 更稳定，不依赖 ICU 区域格式实现 */
      const todayStr = new Date().toLocaleDateString('en-CA', { timeZone: 'Asia/Shanghai' });
      const cursor = getDeviceImapCursor(deviceId);
      if (cursor && cursor.cachedTicketDate === todayStr) {
        const lastSign = getDeviceBitmapSign(deviceId);
        console.log(chalk.gray(`    - 同日重复请求快速路径：cachedTicketDate=${todayStr}，直接返回 needUpdate=false`));
        return makeSuccessReply({
          needUpdate: forceRender,
          hasTicket: !!(ticket && ticket.date),
          sign: lastSign || '',
        });
      }
    }

    /* 4) 渲染位图（ticket 为 null 时 renderTicket 内部回退到 DEFAULT_EMPTY_TICKET）
       始终以 250×122 标准尺寸渲染，再按设备上报的 X-Screen-W/H 缩放 */
    const nowIso = new Date().toISOString();
    const rawBitmap = renderTicket(ticket, { nowIso });
    const hasTicket = !!(ticket && ticket.date);
    const screenW = parseInt(request.headers['x-screen-w'], 10) || SCREEN_W;
    const screenH = parseInt(request.headers['x-screen-h'], 10) || SCREEN_H;
    const bitmap = (screenW !== SCREEN_W || screenH !== SCREEN_H)
      ? scale1bitBitmap(rawBitmap, SCREEN_W, SCREEN_H, screenW, screenH)
      : rawBitmap;

    /* 4.5) 渲染后记录"今天已渲染"标记（用于 3.5 同日重复请求快速路径）
       仅在复用缓存车票时记录即可——新抓取路径已在 getTicket 内更新 cachedTicket，
       此处补 cachedTicketDate 即可让今天后续的失败重试走快速路径 */
    if (fromCache) {
      const todayStr = new Date().toLocaleDateString('en-CA', { timeZone: 'Asia/Shanghai' });
      await setDeviceImapCursor(deviceId, { cachedTicketDate: todayStr });
    }

    /* 5) 计算签名并与上次比对，决定是否需要下发 bitmap */
    const sign = computeBitmapSign(bitmap);
    const lastSign = getDeviceBitmapSign(deviceId);
    const needUpdate = (sign !== lastSign) || forceRender;

    if (needUpdate) {
      // bitmap 有变化（含首次、有票变化、有票↔无票切换）→ 更新签名记录
      const saved = await setDeviceBitmapSign(deviceId, sign);
      if (!saved) {
        console.warn(chalk.yellow(`    ! 签名落盘失败: device=${deviceId}`));
      }
    }
    console.log(chalk.gray(`    - sign=${sign} last=${lastSign || '-'} needUpdate=${needUpdate} hasTicket=${hasTicket}`));

    /* 6) 测试开关：SAVE_PREVIEW=true 时异步保存 PNG 预览（不阻塞响应） */
    if (needUpdate && SAVE_PREVIEW) {
      const ts = new Date().toISOString().replace(/[:.]/g, '-');
      fs.promises.mkdir(PREVIEW_DIR, { recursive: true })
        .then(() => fs.promises.writeFile(path.join(PREVIEW_DIR, `${ts}.png`), bitmapToPng(bitmap)))
        .then(() => console.log(chalk.gray(`    ✓ PNG 预览已保存: tmp-previews/${ts}.png`)))
        .catch((e) => console.warn(chalk.yellow('    ! PNG 预览保存失败: ' + e.message)));
    }

    /* 7) 组装响应
       - hasTicket=true  + needUpdate=true  → 加密下发 bitmap，硬件端刷屏
       - hasTicket=false + needUpdate=true  → 不下发 bitmap，硬件端走 fallback（挂件图/NO TICKET）
       - needUpdate=false → 不下发 bitmap，硬件端保留画面
       这样避免无票时把 DEFAULT_EMPTY_TICKET 的假票先刷上去再被 fallback 覆盖 */
    if (needUpdate && hasTicket) {
      const encryptedBase64Bitmap = encryptBuffer(bitmap, hexKey);
      return makeSuccessReply({
        needUpdate,
        hasTicket,
        sign,
        bitmap: encryptedBase64Bitmap,
        size: bitmap.length,
      });
    }
    return makeSuccessReply({
      needUpdate,
      hasTicket,
      sign,
    });
  });

  // 启动
  const port = parseInt(process.env.PORT || '8080', 10);
  const start = async () => {
    try {
      await fastify.listen({ port, host: '0.0.0.0' });
      console.log(chalk.green(`\n✓ 服务启动成功: http://0.0.0.0:${port}`));
      console.log(chalk.gray(`  POST /key    { deviceId, key }       注册/更新设备 AES 密钥`));
      console.log(chalk.gray(`  POST /ticket (Header: X-Device-Id)  抓票 → 返回 { needUpdate, hasTicket, sign, bitmap? }`));
      console.log(chalk.gray(`    needUpdate=true 时才带 bitmap 字段，硬件端据此决定是否刷新屏幕`));
      console.log(chalk.gray(`  密钥持久化: ${CONFIG_FILE}`));
      console.log(chalk.gray(`  签名持久化: ${CONFIG_FILE} (config.bitmapSigns)`));
    } catch (err) {
      fastify.log.error(err);
      process.exit(1);
    }
  };
  start();
}

/**
 * 入口
 */
function main() {
  startServer();
}

main();
