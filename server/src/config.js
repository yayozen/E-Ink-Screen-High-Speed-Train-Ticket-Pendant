/**
 * 服务端配置 + 持久化（v4）
 *
 * 文件位置：
 *   - BASE_DIR/.env             静态只读配置（URL 模板、IMAP 过滤）
 *   - BASE_DIR/data/datas.json  运行时可写（按 deviceId 注册的 AES 共享密钥 + bitmap 签名）
 *
 * 字段：
 *   .env 读入：
 *     TICKET_SENDERS / FETCH_DAYS  抓取过滤
 *     *_API                        12306 补全接口模板
 *
 *   datas.json 读/写：
 *     aesKeys: { [deviceId]: '32字符hex' }   各设备推送来的 AES-128 共享密钥
 *     bitmapSigns: { [deviceId]: '16字符hex' } 各设备上次下发的 bitmap 签名
 */
const path = require('path');
const fs = require('fs');
require('dotenv').config({ path: path.resolve(__dirname, '..', '.env') });

const BASE_DIR = path.resolve(__dirname, '..');
/** 运行时数据目录（持久化 datas.json 等） */
const DATA_DIR = path.join(BASE_DIR, 'data');
const CONFIG_FILE = path.join(DATA_DIR, 'datas.json');

/* 启动时确保 data 目录存在（同步创建，已存在不报错） */
if (!fs.existsSync(DATA_DIR)) {
  fs.mkdirSync(DATA_DIR, { recursive: true });
}

/* ============================================================
 * .env 静态配置（只读，不持久化到 datas.json）
 * ============================================================ */
const envConfig = {
  /** 抓取过滤 */
  TICKET_SENDERS: (process.env.TICKET_SENDERS || '12306@rails.com.cn')
    .split(',')
    .map((s) => s.trim().toLowerCase())
    .filter(Boolean),
  FETCH_DAYS: parseInt(process.env.FETCH_DAYS || '15', 10),

  /**
   * 测试开关：设为 "true" 时 /ticket 跳过 IMAP 抓取，直接用 DEFAULT_EMPTY_TICKET 渲染
   * 用于本地无 IMAP 凭据时验证渲染/签名/加密链路（生成的 PNG 也可保存）
   */
  MOCK_TICKET: process.env.MOCK_TICKET === 'true',

  /**
   * 测试开关：设为 "true" 时把每次 needUpdate=true 的 bitmap 保存为 PNG 到 tmp-previews/
   * 默认关闭，仅调试渲染效果时开启
   */
  SAVE_PREVIEW: process.env.SAVE_PREVIEW === 'true',

  /**
   * 12306 检票口补全接口模板
   * 当邮件正文中未带"检票口"时，回退到该接口补全。
   * 可用占位符：{trainCode} {stationName} {date}
   */
  CHECK_POSITION_API: process.env.CHECK_POSITION_API
    || 'https://mobile.12306.cn/weixin//wxcore/getPlatform?trainCode={trainCode}&stationName={stationName}&date={date}',

  /**
   * 12306 到达时间补全 —— 第一步
   */
  TRAIN_SEARCH_API: process.env.TRAIN_SEARCH_API
    || 'https://search.12306.cn/search/v1/train/search?keyword={trainCode}&date={date}&type=wx_checi',

  /**
   * 12306 到达时间补全 —— 第二步
   */
  TRAIN_DETAIL_API: process.env.TRAIN_DETAIL_API
    || 'https://mobile.12306.cn/weixin/wxcore/queryByTrainNo?train_no={trainNo}&from_station_telecode=BBB&to_station_telecode=BBB&depart_date={date}',
};

/* ============================================================
 * datas.json 动态配置（运行时可写）
 *
 * 并发安全：loadConfig + 修改 + saveConfig 是 read-modify-write，
 * Node 单线程下 async 请求间仍可能交错（/key 和 /ticket 并发），
 * 用 Promise 链串行化所有写操作，避免丢字段
 * ============================================================ */

/** 读取磁盘配置（纯函数，不改状态） */
function loadConfig() {
  try {
    if (!fs.existsSync(CONFIG_FILE)) {
      return {};
    }
    const raw = fs.readFileSync(CONFIG_FILE, 'utf8');
    const cfg = JSON.parse(raw);
    if (typeof cfg !== 'object' || cfg === null) {
      return {};
    }
    return cfg;
  } catch (e) {
    console.error(`[CFG] datas.json 读取失败: ${e.message}`);
    return {};
  }
}

/** 原子写 datas.json：先写 .tmp 再 rename，避免半写状态 */
function saveConfigSync(cfg) {
  try {
    const tmp = CONFIG_FILE + '.tmp';
    fs.writeFileSync(tmp, JSON.stringify(cfg, null, 2), 'utf8');
    fs.renameSync(tmp, CONFIG_FILE);
    return true;
  } catch (e) {
    console.error(`[CFG] datas.json 写入失败: ${e.message}`);
    return false;
  }
}

/** 写入串行化队列：所有写操作排队执行，避免 read-modify-write 交错 */
let writeQueue = Promise.resolve();

/**
 * 串行化更新 datas.json：传入 mutator 函数拿到当前 cfg，修改后返回
 * 同一时刻只有一个 mutator 在跑，保证并发安全
 * @param {(cfg: object) => boolean} mutator 返回 true 表示需要落盘
 * @returns {Promise<boolean>} 是否落盘成功
 */
function updateConfig(mutator) {
  writeQueue = writeQueue.then(() => {
    const cfg = loadConfig();
    const dirty = mutator(cfg);
    if (dirty) return saveConfigSync(cfg);
    return true;
  }).catch((e) => {
    console.error(`[CFG] updateConfig 异常: ${e.message}`);
    return false;
  });
  return writeQueue;
}

/** 同步版 saveConfig（仅给初始化/脚本使用，运行时请用 updateConfig） */
function saveConfig(cfg) {
  return saveConfigSync(cfg);
}

/**
 * 按 deviceId 查 32 字符 hex 密钥；未注册返回 null
 */
function getDeviceKey(deviceId) {
  if (!deviceId) return null;
  const cfg = loadConfig();
  const k = (cfg.aesKeys || {})[deviceId];
  if (typeof k === 'string' && /^[0-9a-fA-F]{32}$/.test(k)) {
    return k.toLowerCase();
  }
  return null;
}

/**
 * 把 deviceId → 密钥 写入 config.aesKeys 并落盘（异步串行化）
 * @returns {Promise<boolean>} 是否落盘成功
 */
function setDeviceKey(deviceId, hexKey) {
  if (!deviceId || !/^[0-9a-fA-F]{32}$/.test(hexKey)) {
    return Promise.resolve(false);
  }
  const lower = hexKey.toLowerCase();
  return updateConfig((cfg) => {
    cfg.aesKeys = cfg.aesKeys || {};
    if (cfg.aesKeys[deviceId] === lower) return false; // 无变化
    cfg.aesKeys[deviceId] = lower;
    return true;
  });
}

/* ============================================================
 * 设备位图签名（v5：避免重复下发相同 bitmap）
 *   datas.json: { bitmapSigns: { [deviceId]: '16字符hex' } }
 *   抓票流程渲染 bitmap 后计算 SHA-256 前 16 字符作为签名，
 *   与上次比对：相同则只回 status，不下发 bitmap
 * ============================================================ */

/** 签名格式校验：16 字符 hex（SHA-256 前 16 字符） */
const SIGN_RE = /^[0-9a-f]{16}$/;

/**
 * 读取设备上次下发的 bitmap 签名；未记录返回 null
 * @param {string} deviceId
 * @returns {string|null} 16 字符 hex 签名
 */
function getDeviceBitmapSign(deviceId) {
  if (!deviceId) return null;
  const cfg = loadConfig();
  const s = (cfg.bitmapSigns || {})[deviceId];
  if (typeof s === 'string' && SIGN_RE.test(s)) {
    return s;
  }
  return null;
}

/**
 * 把 deviceId → bitmap 签名 写入 config.bitmapSigns 并落盘（异步串行化）
 * @param {string} deviceId
 * @param {string} sign 16 字符 hex 签名
 * @returns {Promise<boolean>} 是否成功
 */
function setDeviceBitmapSign(deviceId, sign) {
  if (!deviceId || !SIGN_RE.test(sign)) {
    return Promise.resolve(false);
  }
  return updateConfig((cfg) => {
    cfg.bitmapSigns = cfg.bitmapSigns || {};
    if (cfg.bitmapSigns[deviceId] === sign) return false; // 无变化
    cfg.bitmapSigns[deviceId] = sign;
    return true;
  });
}

/* ============================================================
 * 设备 IMAP 抓取游标 + 车票缓存（v6：UID 增量抓取优化）
 *   datas.json: { imapCursors: { [deviceId]: {
 *                  lastUid: <number>,            // 已抓取到的最大 UID
 *                  uidValidity: <number>,        // 上次记录的 UIDVALIDITY
 *                  cachedTicket: <object|null>,  // 上次解析出的车票对象
 *                  cachedTicketDate: <string>    // 上次渲染日期 YYYY-MM-DD
 *                } } }
 *
 * 设计依据：日常无新邮件时，IMAP 全量 search + 逐封 fetchOne 是
 *   服务端响应时间的主要来源（~3-5s）。改为：
 *   - 用 UID > lastUid 增量 search；无新邮件直接跳过邮件解析
 *   - 复用 cachedTicket 重新渲染（含倒计时变化时必渲染）
 *   - 同日重复请求（硬件端失败重试）且 cachedTicket 未变 → 跳过渲染，
 *     直接返回 needUpdate=false，硬件端避免无效刷屏
 * ============================================================ */

/**
 * 读取设备 IMAP 抓取游标（lastUid + uidValidity + 车票缓存）
 * 未记录返回 null
 * @param {string} deviceId
 * @returns {{lastUid:number, uidValidity:number, cachedTicket:object|null, cachedTicketDate:string}|null}
 */
function getDeviceImapCursor(deviceId) {
  if (!deviceId) return null;
  const cfg = loadConfig();
  const c = (cfg.imapCursors || {})[deviceId];
  if (!c || typeof c !== 'object') return null;
  return {
    lastUid: Number(c.lastUid) || 0,
    uidValidity: Number(c.uidValidity) || 0,
    cachedTicket: c.cachedTicket || null,
    cachedTicketDate: typeof c.cachedTicketDate === 'string' ? c.cachedTicketDate : '',
  };
}

/**
 * 更新设备 IMAP 抓取游标（增量覆盖，未传字段保持原值）
 * @param {string} deviceId
 * @param {Partial<{lastUid:number, uidValidity:number, cachedTicket:object|null, cachedTicketDate:string}>} patch
 * @returns {Promise<boolean>} 是否落盘成功
 */
function setDeviceImapCursor(deviceId, patch) {
  if (!deviceId || !patch || typeof patch !== 'object') {
    return Promise.resolve(false);
  }
  return updateConfig((cfg) => {
    cfg.imapCursors = cfg.imapCursors || {};
    const cur = cfg.imapCursors[deviceId] || {};
    if (patch.lastUid !== undefined) cur.lastUid = Number(patch.lastUid) || 0;
    if (patch.uidValidity !== undefined) cur.uidValidity = Number(patch.uidValidity) || 0;
    if (patch.cachedTicket !== undefined) cur.cachedTicket = patch.cachedTicket;
    if (patch.cachedTicketDate !== undefined) cur.cachedTicketDate = patch.cachedTicketDate;
    cfg.imapCursors[deviceId] = cur;
    return true;
  });
}

/* ============================================================
 * 导出（兼容旧模块用 envConfig.* 字段）
 * ============================================================ */
module.exports = {
  // 兼容旧用法（arrive-time-fetcher / check-position-fetcher 等）
  ...envConfig,
  BASE_DIR,
  DATA_DIR,

  // 新的动态配置 API
  CONFIG_FILE,
  loadConfig,
  saveConfig,
  updateConfig,
  getDeviceKey,
  setDeviceKey,
  getDeviceBitmapSign,
  setDeviceBitmapSign,
  getDeviceImapCursor,
  setDeviceImapCursor,
};
