/**
 * 12306 购票邮件解析器
 *
 * 邮件主题固定为「网上购票系统-用户支付通知」
 * 邮件正文是单行密文格式：
 *   "尊敬的 沈万三先生： 您于2026年07月06日在...成功购买了1张车票...。
 *    所购车票信息如下：
 *      1.沈万三，2026年07月20日08:13开，合肥南站-上海虹桥站，G7178次列车，5车5F号，二等座，成人票，票价231.0元，检票口1A，电子客票。
 *    温馨提示 ..."
 * 
 * 邮件主题为「网上购票系统-用户退票通知」
 * 邮件正文是单行密文格式：
 *   "您于2026年07月07日在中国铁路客户服务中心网站(12306.cn)成功办理了退票业务， 订单号码 EE27033423...。
 *    所退车票信息如下：
 *      沈万三，2026年07月20日17:34开，上海虹桥站-合肥南站，G2393次列车，5车11F号，二等座，票价239.0元，退票费0.0元，应退票款239.0元。
 *    温馨提示 ..."
 * 
 * 邮件主题为「网上购票系统-用户改签通知」
 * 邮件正文是单行密文格式：
 *   "您于2026年01月09日在中国铁路客户服务中心网站(12306.cn)成功改签车票1张，新车票票款共计208.00元。 订单号码EE10381334...。
 *    改签后的车票信息如下：
 *      1.沈万三，2026年01月16日18:14开，上海虹桥站-合肥站，G1826次列车，11车15F号，二等座，成人票，票价208.0元，检票口7A7B，电子客票。
 *    温馨提示 ..."
 * 
 * 
 * 邮件主题为「网上购票系统-候补订单兑现成功通知」
 * 邮件正文是单行密文格式：
 *   "您在中国铁路客户服务中心网站(12306.cn)成功办理了候补购票业务， 成功兑现了1张车票，票款共计236.00元，订单号码 EE63081983...。
 *    兑现车票信息如下：
 *      1.杨友正，2026年06月18日17:29开，上海站-合肥站，G7256次列车,5车4B号，二等座，票价236.0元，检票口候车室8。
 *    温馨提示 ..."
 *
 */

function pad(n) {
  return String(n).padStart(2, '0');
}

/**
 * 邮件主题分类
 *  - 网上购票系统-用户支付通知 → 购票成功 ✓
 *  - 网上购票系统-用户退票通知 → 退票
 *  - 网上购票系统-用户改签通知 → 改签
 */
const TICKET_SUBJECTS = {
  PURCHASE: '网上购票系统-用户支付通知',
  REFUND: '网上购票系统-用户退票通知',
  CHANGE: '网上购票系统-用户改签通知',
  CHANGE_STATION: '网上购票系统-用户变更到站通知',
  HOOKUP: '网上购票系统-候补订单兑现成功通知',
};

/**
 * 判定是否购票成功通知
 * 严格按主题判断（最稳定，避免正文"换退/退票"等字样误判）
 */
function isTicketPurchaseEmail(subject) {
  if (!subject) return false;
  // 必须完全等于"网上购票系统-用户支付通知"
  return subject.trim() === TICKET_SUBJECTS.PURCHASE;
}

/**
 * 判定是否退票通知
 */
function isTicketRefundEmail(subject) {
  if (!subject) return false;
  // 必须完全等于"网上购票系统-用户退票通知"
  return subject.trim() === TICKET_SUBJECTS.REFUND;
}

/**
 * 判定是否改签通知
 */
function isTicketChangeEmail(subject) {
  if (!subject) return false;
  // 必须完全等于"网上购票系统-用户改签通知"
  return subject.trim() === TICKET_SUBJECTS.CHANGE || subject.trim() === TICKET_SUBJECTS.CHANGE_STATION;
}


/**
 * 判断是否属于解析目标邮件
 */
function isTicketEmail(subject) {
  return Object.values(TICKET_SUBJECTS).includes(subject.trim());
}


/**
 * 解析单张票段（直接对原始文本做正则匹配，不再依赖逗号切分位置）
 */
function parseOneSegment(seg, orderNo) {
  const t = {
    date: '', trainNo: '', fromStation: '', toStation: '',
    departTime: '', arriveTime: '',
    carriage: '', seat: '', seatType: '', passenger: '',
    orderNo: orderNo,
    checkPosition: '', // 检票口
    // 行程耗时
    duration: '',
  };

  // 乘车人：固定在"信息如下："之后（4 种邮件都满足），可能带"1."序号前缀
  // 早期正则 /(?:\d+\.)?([^\s，,]+?)(?=[，,\s]|$)/ 从 seg 起点匹配，
  // 会把前缀 "。所购车票信息如下：1." 一起吞进捕获组，必须用 "信息如下：" 锚定
  const nameM = seg.match(/信息如下[：:]\s*(?:\d+\.)?\s*([^\s，,]+?)(?=[，,\s]|$)/);
  if (nameM) t.passenger = nameM[1];

  // 日期 + 出发时间
  const dateM = seg.match(/(\d{4})\s*年\s*(\d{1,2})\s*月\s*(\d{1,2})\s*日\s*(\d{1,2}):(\d{2})\s*开/);
  if (dateM) {
    t.date = `${dateM[1]}-${pad(dateM[2])}-${pad(dateM[3])}`;
    t.departTime = `${pad(dateM[4])}:${dateM[5]}`;
  }

  // 出发-到达站
  const stationM = seg.match(/([\u4e00-\u9fa5]{2,10}?站)\s*[-—–]\s*([\u4e00-\u9fa5]{2,10}?站)/);
  if (stationM) {
    t.fromStation = stationM[1].replace(/站$/, '');
    t.toStation = stationM[2].replace(/站$/, '');
  }

  // 车次
  const trainM = seg.match(/([GCDZTKSLP1-9]\d{1,4})\s*次列车/);
  if (trainM) t.trainNo = trainM[1];

  // 车厢 + 座位号
  const seatM = seg.match(/(\d{1,2})\s*车\s*(\d{1,3}[A-Fa-f]?)\s*号/);
  if (seatM) {
    t.carriage = seatM[1];
    t.seat = seatM[2];
  }

  // 席别
  const typeM = seg.match(/(二等座|一等座|商务座|特等座|硬座|软座|硬卧|软卧|高级软卧|动卧|二等卧|一等卧|无座|多功能座)/);
  if (typeM) t.seatType = typeM[1];

  // 检票口（独立提取，不受位置影响）
  const checkM = seg.match(/检票口[\dA-F]+/);
  if (checkM) t.checkPosition = checkM[0];

  // 必须有 车次 + 日期
  if (!t.trainNo) return null;
  return t;
}

/**
 * 主入口
 * @returns {object|null} 单张票；解析失败返回 null
 */
function parseTicketFromText(rawText) {
  if (!rawText || typeof rawText !== 'string') return null;

  // 提取订单号
  const orderNo = rawText.match(/EE(\d+)/);
  if (!orderNo) return null;
  const orderNoStr = orderNo[0].trim();
  if (!orderNoStr) return null;
  
  // 正则匹配单张票段（车次号首字母与 parseOneSegment 保持一致：G/C/D/Z/T/K/S/L/P 或数字开头）
  const match = rawText.match(/。.*，?[GCDZTKSLP1-9]\d{1,4}次列车.+车.*?。/);
  if (!match) return {orderNo: orderNoStr};
  const firstTicketSegment = match[0].trim();
  if (!firstTicketSegment) return {orderNo: orderNoStr};
  return parseOneSegment(firstTicketSegment, orderNoStr);
}

/**
 * 剔除所有html标签和换行符
 */
function cleanTicketBody(body) {
  if (!body) return '';
  // 强制“温馨提示”分割截断
  let text = body.split('温馨提示')[0];
  text = text.replace(/<[^>]+>/g, '');
  text = text.replace(/[\n\r\t]/g, '');
  return text;
}

module.exports = {
  parseTicketFromText,
  isTicketPurchaseEmail,
  isTicketRefundEmail,
  isTicketChangeEmail,
  isTicketEmail,
  cleanTicketBody,
};
