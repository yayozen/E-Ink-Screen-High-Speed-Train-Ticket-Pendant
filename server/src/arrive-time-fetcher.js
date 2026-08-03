/**
 * 12306 到达时间补全接口
 *
 * 邮件正文中并不包含"到达时间"字段（只有"HH:MM开"这种出发时间），本模块
 * 通过两个 12306 接口串联补全目的站 arrive_time：
 *
 *   1) search 接口：用车次号（G7178）+ 日期 查到该车次当日的内部 train_no
 *      模板：config.TRAIN_SEARCH_API  —— 占位符 {trainCode} {date}
 *      日期格式：YYYYMMDD（无连字符）
 *   2) queryByTrainNo 接口：用 train_no + 日期 拿到完整经停站列表（含 arrive_time）
 *      模板：config.TRAIN_DETAIL_API  —— 占位符 {trainNo} {date}
 *      日期格式：YYYY-MM-DD
 *
 *   - 接口 2 的 from/to station_telecode 在模板里写死为 'BBB'，
 *     实测不影响返回完整经停站，省去维护站点电码表。
 *
 * 用法：
 *   const { getArriveTime } = require('./arrive-time-fetcher');
 *   const at = await getArriveTime('G7178', '上海虹桥', '2026-07-12');
 *
 * 设计原则：
 *  - 容错优先：网络错误 / 解析失败 / 字段缺失一律返回空串，不抛出
 *  - 调试可观察：失败时打印原始响应，方便定位接口字段
 */
const chalk = require('chalk');
const config = require('./config');

/** 起始站的 arrive_time 占位值（不是真实到达时间），需跳过 */
const START_STATION_ARRIVE_PLACEHOLDER = '----';

/**
 * 用占位符填充接口模板（参考 check-position-fetcher 的写法）
 * 调用方负责把日期先转成目标 API 期望的格式
 */
function fillTemplate(template, params) {
  let url = String(template);
  for (const [key, value] of Object.entries(params)) {
    url = url.split(key).join(encodeURIComponent(value));
  }
  return url;
}

/**
 * 通用 JSON GET：自动超时，失败返回 null
 * @param {string} url 完整 URL（已 encode）
 * @param {number} timeoutMs 超时毫秒
 * @returns {Promise<object|null>}
 */
async function httpGetJson(url, timeoutMs = 5000) {
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
      console.log(chalk.yellow(`    ! 到达时间接口 HTTP ${resp.status}: ${url}`));
      return null;
    }
    const text = await resp.text();
    try { return JSON.parse(text); }
    catch (_) {
      console.log(chalk.yellow(`    ! 到达时间接口返回非 JSON: ${text.slice(0, 120)}`));
      return null;
    }
  } catch (e) {
    clearTimeout(timer);
    console.log(chalk.yellow(`    ! 到达时间接口异常: ${e.message || e}`));
    return null;
  }
}

/**
 * 第一步：调用 search 接口，把外部车次号（G7178）转成内部 train_no（5i000G717801）
 * 优先按 station_train_code 精确匹配；找不到再回退到第一条
 * @param {string} trainNo 车次号，如 G7178
 * @param {string} date YYYY-MM-DD
 * @returns {Promise<string>} train_no；失败返回空串
 */
async function getTrainNo(trainNo, date) {
  if (!trainNo || !date || !config.TRAIN_SEARCH_API) return '';
  // search 接口要 YYYYMMDD（无连字符）
  const dateNoDash = date.replace(/-/g, '');
  const url = fillTemplate(config.TRAIN_SEARCH_API, {
    '{trainCode}': trainNo,
    '{date}': dateNoDash,
  });
  const data = await httpGetJson(url);
  if (!data || !Array.isArray(data.data) || data.data.length === 0) return '';

  const exact = data.data.find((d) => d && d.station_train_code === trainNo);
  const pick = exact || data.data[0];
  return (pick && pick.train_no) ? String(pick.train_no) : '';
}

/**
 * 从 queryByTrainNo 响应中提取经停站数组
 * 响应结构兼容两种：{ data: [...] } 或 { data: { data: [...] } }
 */
function extractStations(routeData) {
  if (!routeData || typeof routeData !== 'object') return [];
  let stations = routeData.data;
  if (stations && !Array.isArray(stations) && Array.isArray(stations.data)) {
    stations = stations.data;
  }
  return Array.isArray(stations) ? stations : [];
}

/**
 * 从经停站列表中按站名查找 arrive_time
 *  - 跳过起始站（arrive_time = '----'）
 *  - 跨日时返回 "HH:MM+N" 形式
 * @param {Array} stations 经停站列表
 * @param {string} stationName 目的站名（不带"站"后缀）
 * @returns {string} 到达时间；找不到返回空串
 */
function findArriveTimeInStations(stations, stationName) {
  if (!Array.isArray(stations) || stations.length === 0 || !stationName) return '';
  for (const st of stations) {
    if (!st || st.station_name !== stationName) continue;
    const arriveTime = (st.arrive_time || '').trim();
    if (!arriveTime || arriveTime === START_STATION_ARRIVE_PLACEHOLDER) return '';
    const dayDiff = parseInt(st.arrive_day_diff, 10) || 0;
    return dayDiff > 0 ? `${arriveTime}+${dayDiff}` : arriveTime;
  }
  return '';
}

/**
 * 主入口：获取指定车次到达 stationName 站的到达时间
 * @param {string} trainNo 车次号，如 G7178
 * @param {string} toStationName 目的站名（不带"站"后缀），如 "上海虹桥"
 * @param {string} date 出行日期 YYYY-MM-DD
 * @returns {Promise<string>} 到达时间字符串；任何失败返回空串
 */
async function getArriveTime(trainNo, toStationName, date) {
  if (!trainNo || !toStationName || !date) return '';

  // 第一步：外部车次 → 内部 train_no
  const trainNoInternal = await getTrainNo(trainNo, date);
  if (!trainNoInternal) {
    console.log(chalk.yellow(`    ! 到达时间：未找到 ${trainNo} 在 ${date} 的 train_no`));
    return '';
  }

  // 第二步：train_no + 日期 → 经停站列表
  if (!config.TRAIN_DETAIL_API) return '';
  const url = fillTemplate(config.TRAIN_DETAIL_API, {
    '{trainNo}': trainNoInternal,
    '{date}': date, // queryByTrainNo 要 YYYY-MM-DD
  });
  const routeData = await httpGetJson(url);
  if (!routeData) return '';

  const stations = extractStations(routeData);
  if (stations.length === 0) {
    console.log(chalk.yellow(`    ! 到达时间：${trainNoInternal} 经停站为空`));
    return '';
  }
  return findArriveTimeInStations(stations, toStationName);
}

module.exports = {
  getArriveTime,
  getTrainNo,
  extractStations,
  findArriveTimeInStations,
};
