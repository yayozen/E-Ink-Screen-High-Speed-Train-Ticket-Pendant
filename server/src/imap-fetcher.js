/**
 * IMAP 抓取模块（v6：UID 增量 + 车票缓存）
 *
 * 从 index.js 抽出，便于 test-finder 直接复用真实抓取流程做集成测试。
 *
 * 三个层次：
 *   1. fetchBody / fetchEnvelope  —— 单封邮件抓取（全 body / 仅信封头）
 *   2. fetchAllMails / fetchIncrementalEnvelopes  —— 批量抓取（全量 / 增量）
 *   3. getTicket  —— 顶层入口，含 UIDVALIDITY 比对 + 缓存策略
 *
 * 依赖：imapflow + mailparser + ticket-finder + parser + config（IMAP 游标）
 */
const { ImapFlow } = require('imapflow');
const { simpleParser } = require('mailparser');
const chalk = require('chalk');
const { findNextTicket } = require('./ticket-finder');
const { cleanTicketBody, isTicketEmail } = require('./parser');
const {
  getDeviceImapCursor,
  setDeviceImapCursor,
  TICKET_SENDERS,
} = require('./config');

/**
 * 构造"几天前 00:00"的 Date，用于 IMAP SINCE 过滤
 * @param {number} daysAgo
 * @returns {Date}
 */
function makeSinceDate(daysAgo) {
  const d = new Date();
  d.setDate(d.getDate() - daysAgo);
  d.setHours(0, 0, 0, 0);
  return d;
}

/**
 * 取单封邮件的完整 body（按 seq 或 UID）
 * @param {ImapFlow} client
 * @param {number} seqOrUid 序号或 UID
 * @param {boolean} useUid true 时 seqOrUid 视为 UID（与 search 返回值一致）
 * @returns {Promise<object>} { uid, seq, subject, fromAddr, fromName, date, body }
 */
async function fetchBody(client, seqOrUid, useUid) {
  const fetch = await client.fetchOne(seqOrUid, {
    source: true,
    envelope: true,
    uid: true,
  }, { uid: !!useUid });

  const parsed = await simpleParser(fetch.source);
  const body = cleanTicketBody(parsed.text || parsed.textAsHtml || parsed.html || '');
  return {
    uid: fetch.uid,
    seq: seqOrUid,
    subject: parsed.subject || '',
    fromAddr: (parsed.from && parsed.from.value && parsed.from.value[0] && parsed.from.value[0].address || '').toLowerCase(),
    fromName: (parsed.from && parsed.from.value && parsed.from.value[0] && parsed.from.value[0].name) || '',
    date: parsed.date || null,
    body,
  };
}

/**
 * 取单封邮件的 envelope（仅信封头，不取 body）—— 比 fetchBody 快一个数量级
 * 用于 UID 增量快速判断"新邮件中是否含车票相关邮件"
 * @param {ImapFlow} client
 * @param {number} uid 邮件 UID
 * @returns {Promise<object>} { uid, subject, fromAddr, date }
 */
async function fetchEnvelope(client, uid) {
  const fetch = await client.fetchOne(uid, { envelope: true, uid: true }, { uid: true });
  const env = fetch.envelope || {};
  return {
    uid: fetch.uid,
    subject: env.subject || '',
    fromAddr: (env.from && env.from[0] && env.from[0].address || '').toLowerCase(),
    date: env.date || null,
  };
}

/**
 * UID 增量抓取：返回新邮件的 envelope 数组 + 最大 UID + 是否含车票邮件
 *
 * 性能要点：
 *   - 仅 fetch envelope（信封头，不取 body），单封耗时 ~5ms
 *   - 用 subject 关键词快速判断是否含车票邮件
 *   - 调用方据此决定：无车票邮件 → 复用缓存；有车票邮件 → 全量重抓
 *
 * @param {ImapFlow} client
 * @param {number} lastUid 上次抓取到的最大 UID；0 表示首次（调用方应走全量分支）
 * @returns {Promise<{envelopes: Array, maxUid: number, hasTicketMail: boolean}>}
 */
async function fetchIncrementalEnvelopes(client, lastUid) {
  const lock = await client.getMailboxLock('INBOX');
  try {
    // IMAP `UID X:*` 范围语义：X > 当前最大 UID 时仍返回当前最大 UID 的邮件，
    // 必须 filter 掉 <= lastUid 的，避免重复处理
    const ids = await client.search({ uid: `${lastUid + 1}:*` }, { uid: true });
    const newUids = ids.filter((u) => u > lastUid);
    console.log(chalk.green(`    ✓ UID 增量命中 ${newUids.length} 封新邮件（lastUid=${lastUid}）`));
    if (newUids.length === 0) {
      return { envelopes: [], maxUid: lastUid, hasTicketMail: false };
    }

    const envelopes = [];
    let maxUid = lastUid;
    let hasTicketMail = false;
    for (const uid of newUids) {
      const env = await fetchEnvelope(client, uid);
      envelopes.push(env);
      if (uid > maxUid) maxUid = uid;
      if (isTicketEmail(env.subject)) hasTicketMail = true;
    }
    return { envelopes, maxUid, hasTicketMail };
  } finally {
    await lock.release();
  }
}

/**
 * 全量抓取（按 since 过滤）—— 增量路径检测到车票邮件后回退到此函数
 * 拿到全部命中邮件的 body，交给 findNextTicket 重新筛选最近车票
 *
 * @param {ImapFlow} client
 * @param {Date} since 起始日期
 * @param {string} sender 发件人过滤（163 邮箱不使用）
 * @param {boolean} is163 163 邮箱标志（关闭 from/body 过滤）
 * @returns {Promise<Array>} 邮件数组（含 body 字段）
 */
async function fetchAllMails(client, since, sender, is163) {
  const lock = await client.getMailboxLock('INBOX');
  const mails = [];
  try {
    // 163 邮箱 IMAP SEARCH 不支持带字符串参数的条件（FROM/ SUBJECT/ TEXT），
    // 只能用 SINCE 做基础过滤，发件人在客户端逐封比对；
    // 126 邮箱支持 FROM 完整地址 + body 子串，可服务端直接过滤
    const searchCriteria = is163 ? { since } : { since, from: sender, body: '订单号码' };
    const ids = await client.search(searchCriteria);
    console.log(chalk.green(`    ✓ 全量命中 ${ids.length} 封邮件`));
    if (ids.length === 0) return mails;
    for (const seq of ids) {
      mails.push(await fetchBody(client, seq, false));
    }
  } finally {
    await lock.release();
  }
  return mails;
}

/**
 * 取当前邮箱的 UIDVALIDITY（用于检测邮箱是否被重建）
 * 邮箱重建后 UID 会从 1 重新计数，UIDVALIDITY 值会变，
 * 此时缓存的 lastUid 失效，必须重置为 0 走全量抓取
 * @param {ImapFlow} client
 * @returns {Promise<number>} UIDVALIDITY 值
 */
async function getMailboxUidValidity(client) {
  const status = await client.status('INBOX', { uidValidity: true });
  return Number(status.uidValidity) || 0;
}

/**
 * 抓取最近出行车票（v6：UID 增量 + 车票缓存）
 *
 * 三态返回：
 *   - { ticket: <object>, fromCache: false }：新抓取的车票
 *   - { ticket: <object>, fromCache: true }：复用缓存的车票（无新邮件）
 *   - { ticket: null, fromCache: false }：无车票
 *
 * @param {object} imapCfg IMAP 配置 { user, pass, host, port, useSecure }
 * @param {string} deviceId 设备 ID（用于读写游标缓存）
 * @param {boolean} [force=false] 强制刷新：跳过 UID 增量 + 车票缓存，直接全量重抓
 * @returns {Promise<{ticket: object|null, fromCache: boolean}>}
 * @throws {Error} IMAP 连接 / 认证 / 网络错误时抛出
 */
async function getTicket(imapCfg, deviceId, force = false) {
  if (!imapCfg || !imapCfg.user || !imapCfg.pass || !imapCfg.host) {
    throw new Error('imapCfg 缺少必填字段 user/pass/host');
  }

  const is163 = imapCfg.user.includes('@163.com');
  const port = parseInt(imapCfg.port, 10) || 993;
  const useSecure = imapCfg.useSecure !== false;
  const sender = TICKET_SENDERS[0] || '12306@rails.com.cn';
  const fetchDays = 15;

  console.log(chalk.gray(`    - IMAP: ${imapCfg.user} @ ${imapCfg.host}:${port} (ssl=${useSecure}${force ? ', force=true' : ''})`));

  const client = new ImapFlow({
    host: imapCfg.host,
    port,
    secure: useSecure,
    auth: { user: imapCfg.user, pass: imapCfg.pass },
    logger: false,
  });

  await client.connect();
  console.log(chalk.green('    ✓ IMAP 连接成功'));

  try {
    /* 1) 比对 UIDVALIDITY：变了则重置 lastUid，避免使用失效的游标 */
    const cursor = getDeviceImapCursor(deviceId) || { lastUid: 0, uidValidity: 0, cachedTicket: null };
    const currentUidValidity = await getMailboxUidValidity(client);
    let lastUid = cursor.lastUid;
    if (cursor.uidValidity && currentUidValidity && cursor.uidValidity !== currentUidValidity) {
      console.log(chalk.yellow(`    ! UIDVALIDITY 变化 ${cursor.uidValidity} → ${currentUidValidity}，重置游标`));
      lastUid = 0;
    }

    /* force=true 时强制走全量重抓（用户在配网页面点了"刷新车票"），
       跳过 UID 增量 + 车票缓存路径，确保拿到最新邮件 */
    if (force) {
      console.log(chalk.yellow('    ! force=true，跳过缓存，全量重抓'));
      lastUid = 0;
    }

    /* 2) 首次抓取（lastUid=0）：直接走全量，建立基线游标 */
    if (lastUid === 0) {
      console.log(chalk.gray('    - 首次抓取，走全量路径建立基线'));
      const since = makeSinceDate(fetchDays);
      const mails = await fetchAllMails(client, since, sender, is163);
      const now = Date.now();
      const ticket = await findNextTicket(mails, { now });

      /* 计算本批邮件的最大 UID（mails 数组里的 uid 字段）作为下次游标 */
      let maxUid = 0;
      for (const m of mails) if (m.uid > maxUid) maxUid = m.uid;
      if (maxUid === 0) {
        /* 全量 search 返回 seq 而非 uid，需要再 fetch 一次拿 uid；
           但通常 mails 不会为空到这里，留作兜底：用 mailbox 上的 uidNext - 1 */
        const st = await client.status('INBOX', { uidNext: true });
        maxUid = Math.max(0, Number(st.uidNext) - 1);
      }

      await setDeviceImapCursor(deviceId, {
        lastUid: maxUid,
        uidValidity: currentUidValidity,
        cachedTicket: ticket,
      });
      console.log(chalk.gray(`    - 基线游标已建立：lastUid=${maxUid} uidValidity=${currentUidValidity}`));
      return { ticket, fromCache: false };
    }

    /* 3) 增量路径：仅取 envelope，判断是否含车票邮件 */
    const { envelopes, maxUid, hasTicketMail } = await fetchIncrementalEnvelopes(client, lastUid);

    /* 3a) 无新邮件 → 复用缓存车票（最常见场景，省 simpleParser + findNextTicket） */
    if (envelopes.length === 0) {
      console.log(chalk.green('    ✓ 无新邮件，复用缓存车票'));
      return { ticket: cursor.cachedTicket, fromCache: true };
    }

    /* 3b) 有新邮件但都不是车票 → 更新 lastUid，复用缓存车票 */
    if (!hasTicketMail) {
      console.log(chalk.green(`    ✓ ${envelopes.length} 封新邮件均非车票，复用缓存车票`));
      await setDeviceImapCursor(deviceId, {
        lastUid: maxUid,
        uidValidity: currentUidValidity,
        /* cachedTicket 保持不变 */
      });
      return { ticket: cursor.cachedTicket, fromCache: true };
    }

    /* 3c) 增量邮件中含车票邮件 → 必须全量重抓 + 重新解析，保证退票/改签/新购票正确处理 */
    console.log(chalk.yellow('    ! 检测到新车票邮件，全量重抓 + 重新解析'));
    const since = makeSinceDate(fetchDays);
    const mails = await fetchAllMails(client, since, sender, is163);
    const now = Date.now();
    const ticket = await findNextTicket(mails, { now });

    /* 更新游标 + 车票缓存。maxUid 用全量抓取里最大的 UID（覆盖增量阶段拿到的值，
       避免增量阶段漏抓某些 UID 的边界问题） */
    let fullMaxUid = maxUid;
    for (const m of mails) if (m.uid > fullMaxUid) fullMaxUid = m.uid;
    await setDeviceImapCursor(deviceId, {
      lastUid: fullMaxUid,
      uidValidity: currentUidValidity,
      cachedTicket: ticket,
    });
    return { ticket, fromCache: false };
  } finally {
    try { await client.logout(); } catch (_) {}
  }
}

module.exports = {
  makeSinceDate,
  fetchBody,
  fetchEnvelope,
  fetchIncrementalEnvelopes,
  fetchAllMails,
  getMailboxUidValidity,
  getTicket,
};
