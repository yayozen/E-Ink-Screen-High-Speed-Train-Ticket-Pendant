/**
 * 12306 检票口补全接口
 *
 * 邮件正文中的"检票口"字段偶尔会缺失（特别当出行日临近、12306 还未来得及排
 * 站台时）。本模块通过 12306 移动端 getPlatform 接口补全该字段。
 *
 * 用法：
 *   const { getCheckPosition } = require('./check-position-fetcher');
 *   const pos = await getCheckPosition('G1826', '上海虹桥', '2026-07-10');
 *
 * 设计原则：
 *  - 容错优先：网络错误 / 解析失败 / 字段缺失一律返回空串，不抛出
 *  - 字段路径宽松：尝试多种 12306 移动端常见响应结构
 *  - 调试可观察：失败时打印原始响应，方便定位接口字段
 */
const chalk = require('chalk');
const config = require('./config');

/** 12306 移动端响应中常见的检票口/站台字段名 */
const FIELD_CANDIDATES = [
  'checkPosition', 'checkGate', 'gate', 'boardingGate', 'checkInGate',
  'platform', 'platformNo', 'platformName', 'waitPosition',
];

/** 检票口文本字段名：12306 getPlatform 接口常把整段话放在 result 中 */
const TEXT_RESULT_FIELDS = ['result', 'message', 'msg', 'dataStr'];

/** 在 result 文本中匹配"检票口7A7B"、"检票口1B"等形式 */
function extractFromResultText(text) {
  if (typeof text !== 'string') return '';
  // 1. 优先匹配 "检票口" 开头 + 字母数字组合（覆盖 检票口7A7B、检票口1B、检票口21）
  const m = text.match(/检票口[\dA-Za-z]+/);
  if (m) return m[0];
  return '';
}

/**
 * 从 12306 响应对象中尝试提取检票口字符串
 * 优先看 data.*；若 data 是数组则取 data[0]；兜底看顶层
 */
function extractFromJson(data) {
  if (!data || typeof data !== 'object') return '';

  // 仅在 12306 明确给出"失败"信号时才放弃；缺失 status 时不要误判
  const explicitFail =
    data.status === false ||
    data.status === 0 ||
    (typeof data.httpstatus === 'number' && data.httpstatus >= 400) ||
    (typeof data.code === 'number' && data.code >= 400);
  if (explicitFail) return '';

  const buckets = [];
  if (data.data && typeof data.data === 'object') {
    if (Array.isArray(data.data) && data.data.length > 0) {
      buckets.push(data.data[0]);
    } else {
      buckets.push(data.data);
    }
  }
  buckets.push(data);

  // 先在结构化字段中找
  for (const obj of buckets) {
    if (!obj || typeof obj !== 'object') continue;
    for (const key of FIELD_CANDIDATES) {
      const v = obj[key];
      if (typeof v === 'string' && v.trim()) return v.trim();
      if (v && typeof v === 'object' && typeof v.value === 'string' && v.value.trim()) {
        return v.value.trim();
      }
    }
    if (Array.isArray(obj.list) && obj.list.length > 0) {
      for (const key of FIELD_CANDIDATES) {
        const v = obj.list[0][key];
        if (typeof v === 'string' && v.trim()) return v.trim();
      }
    }
  }

  // 再在 result/message 这类文本字段里正则提取"检票口X"
  for (const obj of buckets) {
    if (!obj || typeof obj !== 'object') continue;
    for (const key of TEXT_RESULT_FIELDS) {
      const v = obj[key];
      if (typeof v === 'string' && v.includes('检票口')) {
        const got = extractFromResultText(v);
        if (got) return got;
      }
    }
  }
  return '';
}

/**
 * 调用 12306 检票口补全接口
 * @param {string} trainNo 车次，如 G1826
 * @param {string} stationName 出发站名（不带"站"后缀），如 "上海虹桥"
 * @param {string} date 出行日期 YYYY-MM-DD
 * @param {Object} [opts]
 * @param {number} [opts.timeoutMs=5000] 请求超时
 * @returns {Promise<string>} 检票口字符串；任何失败返回空串
 */
async function getCheckPosition(trainNo, stationName, date, opts = {}) {
  if (!trainNo || !stationName || !date) return '';
  const template = config.CHECK_POSITION_API;
  if (!template) return '';

  const url = String(template)
    .replace('{trainCode}', encodeURIComponent(trainNo))
    .replace('{stationName}', encodeURIComponent(stationName))
    .replace('{date}', encodeURIComponent(date));

  const timeoutMs = opts.timeoutMs || 5000;
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);

  try {
    const resp = await fetch(url, {
      method: 'GET',
      headers: {
        'User-Agent': 'Mozilla/5.0 (iPhone; CPU iPhone OS 16_0 like Mac OS X) AppleWebKit/605.1.15',
        'Accept': 'application/json,text/plain,*/*',
      },
      signal: controller.signal,
    });
    clearTimeout(timer);
    if (!resp.ok) {
      console.log(chalk.yellow(`    ! 检票口接口 HTTP ${resp.status}: ${url}`));
      return '';
    }
    const text = await resp.text();
    let data;
    try { data = JSON.parse(text); }
    catch (_) {
      console.log(chalk.yellow(`    ! 检票口接口返回非 JSON: ${text.slice(0, 120)}`));
      return '';
    }
    const pos = extractFromJson(data);
    if (!pos) {
      console.log(chalk.yellow(`    ! 检票口接口未识别字段: ${JSON.stringify(data).slice(0, 200)}`));
    }
    return pos;
  } catch (e) {
    clearTimeout(timer);
    console.log(chalk.yellow(`    ! 检票口接口异常: ${e.message || e}`));
    return '';
  }
}

module.exports = {
  getCheckPosition,
  extractFromJson,
};
