/**
 * IMAP 真实抓取集成测试
 *
 * 复用生产代码 imap-fetcher.js 的 getTicket()，验证：
 *   1. .env 中的 IMAP 凭据能正常连接
 *   2. 首次抓取建立基线游标（全量路径）
 *   3. 第二次抓取走增量路径，命中"无新邮件 → 复用 cachedTicket"
 *   4. 渲染位图 + 计算签名 + 比对（与 index.js /ticket 路由一致）
 *
 * 用法：
 *   cd server
 *   npm run test:finder     # 见 package.json scripts
 *   # 或直接：node src/test-finder.js
 *
 * 必要的 .env 字段：
 *   IMAP_USER / IMAP_PASS / IMAP_HOST / IMAP_PORT / USE_SECURE
 *
 * 注意：本脚本会真实连接 IMAP 服务器并写 datas.json（用 test-finder-<user> 作为
 *   deviceId），不会发送 HTTP 请求，不会触碰真实设备的 datas.json 数据。
 */
const path = require('path');
const crypto = require('crypto');
require('dotenv').config({ path: path.resolve(__dirname, '..', '.env') });
const chalk = require('chalk');
const { getTicket } = require('./imap-fetcher');
const { renderTicket, bitmapToPng } = require('./render-bitmap');
const {
  getDeviceImapCursor,
  getDeviceBitmapSign,
  setDeviceBitmapSign,
} = require('./config');

/**
 * 从 .env 构造 IMAP 配置对象
 * 字段顺序与固件端 enstr 解密后的明文格式一致：user|pass|host|port|useSecure
 * @returns {{user:string, pass:string, host:string, port:string, useSecure:boolean}}
 */
function buildImapCfgFromEnv() {
  const user = process.env.IMAP_USER;
  const pass = process.env.IMAP_PASS;
  const host = process.env.IMAP_HOST;
  const port = process.env.IMAP_PORT || '993';
  const useSecure = process.env.USE_SECURE !== 'false';
  if (!user || !pass || !host) {
    throw new Error('.env 缺少 IMAP_USER / IMAP_PASS / IMAP_HOST');
  }
  return { user, pass, host, port, useSecure };
}

/**
 * 计算 1bit bitmap 的 SHA-256 签名（取前 16 字符 hex）
 * 与 index.js computeBitmapSign 完全一致
 * @param {Buffer} bitmap
 * @returns {string}
 */
function computeBitmapSign(bitmap) {
  return crypto.createHash('sha256').update(bitmap).digest('hex').slice(0, 16);
}

/**
 * 打印车票摘要
 * @param {object|null} ticket
 */
function dumpTicket(ticket) {
  if (!ticket) {
    console.log(chalk.yellow('    - 无车票'));
    return;
  }
  console.log(chalk.green('    ✓ 车票信息:'));
  console.log(`        车次:     ${chalk.bold(ticket.trainNo || '-')}`);
  console.log(`        日期:     ${chalk.bold(ticket.date || '-')}`);
  console.log(`        行程:     ${ticket.fromStation || '-'}  →  ${ticket.toStation || '-'}`);
  console.log(`        时间:     ${ticket.departTime || '-'}  →  ${ticket.arriveTime || '-'}`);
  console.log(`        检票口:   ${ticket.checkPosition || '-'}`);
  if (ticket.orderNo) console.log(`        订单号:   ${ticket.orderNo}`);
  if (ticket.carriage || ticket.seat) {
    console.log(`        车厢座位: ${ticket.carriage || '-'}车 ${ticket.seat || '-'}号 (${ticket.seatType || '-'})`);
  }
  if (ticket.passenger) console.log(`        乘车人:   ${ticket.passenger}`);
}

/**
 * 把位图存为 PNG（调试用，方便肉眼看渲染效果）
 * @param {Buffer} bitmap
 * @param {string} tag 文件名标签
 */
function savePreview(bitmap, tag) {
  const fs = require('fs');
  const dir = path.resolve(__dirname, '..', 'tmp-previews');
  fs.mkdirSync(dir, { recursive: true });
  const ts = new Date().toISOString().replace(/[:.]/g, '-');
  const file = path.join(dir, `${ts}-${tag}.png`);
  fs.writeFileSync(file, bitmapToPng(bitmap));
  console.log(chalk.gray(`    ✓ PNG 预览: ${path.relative(process.cwd(), file)}`));
}

/**
 * 主流程：跑两轮 getTicket，验证 v6 增量抓取 + 缓存逻辑
 */
async function main() {
  const imapCfg = buildImapCfgFromEnv();
  /* 用邮箱地址构造 deviceId，避免与真实设备 datas.json 冲突 */
  const deviceId = `test-finder-${imapCfg.user.split('@')[0]}`;
  console.log(chalk.cyan(`\n=== IMAP 集成测试 (deviceId=${deviceId}) ===\n`));

  /* ---------- 第一轮：首次抓取，建立基线游标 ---------- */
  console.log(chalk.cyan('--- 第 1 轮：首次抓取 ---'));
  const t0 = Date.now();
  const r1 = await getTicket(imapCfg, deviceId);
  const dt1 = Date.now() - t0;
  console.log(chalk.gray(`    耗时: ${dt1}ms, fromCache=${r1.fromCache}`));
  dumpTicket(r1.ticket);

  const cursor1 = getDeviceImapCursor(deviceId);
  console.log(chalk.gray(`    游标: lastUid=${cursor1?.lastUid} uidValidity=${cursor1?.uidValidity} cachedTicketDate=${cursor1?.cachedTicketDate}`));

  /* 渲染 + 签名（与 index.js /ticket 路由一致） */
  const bitmap1 = renderTicket(r1.ticket, { nowIso: new Date().toISOString() });
  const sign1 = computeBitmapSign(bitmap1);
  const lastSign1 = getDeviceBitmapSign(deviceId);
  const needUpdate1 = sign1 !== lastSign1;
  console.log(chalk.gray(`    渲染: sign=${sign1} last=${lastSign1 || '-'} needUpdate=${needUpdate1} hasTicket=${!!(r1.ticket && r1.ticket.date)}`));
  if (needUpdate1) {
    await setDeviceBitmapSign(deviceId, sign1);
    savePreview(bitmap1, 'round1');
  }

  /* ---------- 第二轮：应走增量路径，无新邮件 → 复用缓存 ---------- */
  console.log(chalk.cyan('\n--- 第 2 轮：增量抓取（应复用缓存） ---'));
  const t1 = Date.now();
  const r2 = await getTicket(imapCfg, deviceId);
  const dt2 = Date.now() - t1;
  console.log(chalk.gray(`    耗时: ${dt2}ms, fromCache=${r2.fromCache}`));
  dumpTicket(r2.ticket);

  const cursor2 = getDeviceImapCursor(deviceId);
  console.log(chalk.gray(`    游标: lastUid=${cursor2?.lastUid} uidValidity=${cursor2?.uidValidity} cachedTicketDate=${cursor2?.cachedTicketDate}`));

  /* 渲染 + 签名比对 */
  const bitmap2 = renderTicket(r2.ticket, { nowIso: new Date().toISOString() });
  const sign2 = computeBitmapSign(bitmap2);
  const lastSign2 = getDeviceBitmapSign(deviceId);
  const needUpdate2 = sign2 !== lastSign2;
  console.log(chalk.gray(`    渲染: sign=${sign2} last=${lastSign2 || '-'} needUpdate=${needUpdate2} hasTicket=${!!(r2.ticket && r2.ticket.date)}`));

  /* ---------- 断言 ---------- */
  console.log(chalk.cyan('\n=== 测试结论 ==='));
  const checks = [
    { name: '第 1 轮抓取成功', pass: !!r1.ticket || r1.fromCache === false },
    { name: '第 1 轮建立游标', pass: !!cursor1 && cursor1.lastUid > 0 },
    { name: '第 2 轮走缓存路径', pass: r2.fromCache === true },
    { name: '第 2 轮响应快于第 1 轮', pass: dt2 < dt1 },
    { name: '两轮车票一致', pass: JSON.stringify(r1.ticket) === JSON.stringify(r2.ticket) },
  ];
  let allPass = true;
  for (const c of checks) {
    const mark = c.pass ? chalk.green('✓') : chalk.red('✗');
    console.log(`  ${mark} ${c.name}`);
    if (!c.pass) allPass = false;
  }
  console.log('');
  if (allPass) {
    console.log(chalk.green('✅ PASS: v6 增量抓取 + 缓存逻辑工作正常\n'));
    process.exit(0);
  } else {
    console.log(chalk.red('❌ FAIL: 部分检查未通过\n'));
    process.exit(1);
  }
}

main().catch((err) => {
  console.error(chalk.red('\n运行出错:'), err.message);
  console.error(err.stack);
  process.exit(1);
});
