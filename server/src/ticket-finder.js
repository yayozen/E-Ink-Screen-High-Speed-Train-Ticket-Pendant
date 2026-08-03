/**
 * 车票筛选主流程
 *
 * 与 IMAP 抓取解耦：只依赖"邮件数组（subject + body）"即可运行，
 * 方便使用静态 mock 数据进行单元测试。
 *
 * 输入：邮件数组（每封邮件至少包含 subject、body 字段）
 * 输出：最近待出行的有效车次；若没有则返回 null
 *
 * 业务规则（与原 fetch.js 保持一致）：
 *  1. 倒序处理邮件（IMAP 返回按时间正序，倒序后越靠后越新）
 *  2. 退票邮件只记录其订单号到 refundOrderNos，遇到该订单号的有效购票票则跳过
 *  3. 改签邮件既是一条有效购票记录，也要记录其订单号到 changeOrderNos
 *     （倒序排序后改签订单排在原订单前面，先记录改签，再走到原订单时跳过）
 *  4. 过滤掉 date < now 的过期车票
 *  5. 在所有有效车票中取"日期最小"的一张，即"最近待出行"
 */
const { parseTicketFromText, isTicketRefundEmail, isTicketChangeEmail, isTicketEmail } = require('./parser');
const { getCheckPosition } = require('./check-position-fetcher');
const { getArriveTime } = require('./arrive-time-fetcher');

/**
 * 将 "HH:MM" 或 "HH:MM+N"（跨日）格式的时间转换为相对当日的分钟数
 * @param {string} timeStr 时间字符串，如 "08:13" 或 "10:30+1"
 * @returns {{minutes: number, dayDiff: number}|null} 距当日 00:00 的分钟数 + 跨天天数；解析失败返回 null
 */
function parseTimeStr(timeStr) {
  if (!timeStr || typeof timeStr !== 'string') return null;
  const m = timeStr.match(/^(\d{1,2}):(\d{2})(?:\+(\d+))?$/);
  if (!m) return null;
  const minutes = parseInt(m[1], 10) * 60 + parseInt(m[2], 10);
  const dayDiff = m[3] ? parseInt(m[3], 10) : 0;
  return { minutes, dayDiff };
}

/**
 * 计算出发时间到到达时间的行程耗时（分钟）
 * @param {string} departStr 出发时间 "HH:MM"
 * @param {string} arriveStr 到达时间 "HH:MM" 或 "HH:MM+N"
 * @returns {number|null} 行程分钟数；解析失败返回 null
 */
function parseTimeToMinutes(departStr, arriveStr) {
  const dep = parseTimeStr(departStr);
  const arr = parseTimeStr(arriveStr);
  if (!dep || !arr) return null;
  return (arr.dayDiff * 24 * 60 + arr.minutes) - dep.minutes;
}

/**
 * 从邮件数组中找出最近待出行的有效车次
 * @param {Array<{subject: string, body: string}>} mails 邮件列表（按时间正序）
 * @param {Object} [options]
 * @param {number} [options.now] 当前时间毫秒戳；不传则用 Date.now()
 * @returns {Object|null} 最近待出行车次；找不到返回 null
 */
async function findNextTicket(mails, options = {}) {
  if (!Array.isArray(mails) || mails.length === 0) return null;

  const now = typeof options.now === 'number' ? options.now : Date.now();

  // 倒序处理：倒序后索引越大代表邮件越新
  const ordered = [...mails].reverse();

  // 退票订单号集合：遇到这些订单号的有效购票需要跳过
  const refundOrderNos = new Set();
  // 改签订单号集合：先收集改签，再跳过原订单（倒序后改签会先于原订单出现）
  const changeOrderNos = new Set();

  let nextTicket = null;

  for (const mail of ordered) {
    if (!mail || typeof mail.body !== 'string') continue;

    const tk = parseTicketFromText(mail.body);
    if (!tk || !tk.orderNo) continue;

    const subject = mail.subject || '';

    // 过滤非车票邮件
    if (!isTicketEmail(subject)) continue;

    // 退票邮件：仅记录订单号，不参与"最近待出行"选择
    if (isTicketRefundEmail(subject)) {
      refundOrderNos.add(tk.orderNo);
      continue;
    }

    // 跳过已被退票或已被改签的订单
    if (refundOrderNos.has(tk.orderNo) || changeOrderNos.has(tk.orderNo)) continue;

    // 过滤过期车票
    if (tk.date) {
      const ts = new Date(`${tk.date}T${tk.departTime || '00:00'}:00+08:00`).getTime();
      if (Number.isFinite(ts) && ts < now) continue;
    }

    // 取日期最小（即最近待出行）的有效车票
    if (!nextTicket || tk.date < nextTicket.date) {
      nextTicket = tk;
    }

    // 改签邮件：当前这条算有效购票，同时把它的订单号加入"改签集合"，
    // 这样倒序走到原订单时会被过滤掉
    if (isTicketChangeEmail(subject)) {
      changeOrderNos.add(tk.orderNo);
    }
  }

  // 检票口补全：邮件正文未带时回退到 12306 接口
  if (nextTicket && !nextTicket.checkPosition && nextTicket.fromStation) {
    const station = nextTicket.fromStation;
    const fetched = await getCheckPosition(nextTicket.trainNo, station, nextTicket.date);
    if (fetched) {
      nextTicket.checkPosition = fetched;
      console.log((`✓ 检票口补全: ${fetched}（来自接口）`));
    }
  }

  // 到达时间补全：邮件正文无该字段时回退到 12306 接口（search → queryByTrainNo）
  if (nextTicket && !nextTicket.arriveTime && nextTicket.toStation) {
    const fetched = await getArriveTime(nextTicket.trainNo, nextTicket.toStation, nextTicket.date);
    if (fetched) {
      nextTicket.arriveTime = fetched;
      console.log((`✓ 到达时间补全: ${fetched}（来自接口）`));
    }
  }

  // 行程耗时计算
  // arriveTime/departTime 格式为 "HH:MM" 或 "HH:MM+N"（跨日），不能直接 new Date()
  if (nextTicket && nextTicket.arriveTime && nextTicket.departTime) {
    const minutes = parseTimeToMinutes(nextTicket.departTime, nextTicket.arriveTime);
    if (minutes != null && minutes > 0) {
      if (minutes > 60) {
        const hours = minutes / 60;
        nextTicket.duration = `${hours.toFixed(2)} 小时`;
      } else {
        nextTicket.duration = `${minutes.toFixed(0)} 分钟`;
      }
    }
  }

  return nextTicket;
}

module.exports = {
  findNextTicket,
};
