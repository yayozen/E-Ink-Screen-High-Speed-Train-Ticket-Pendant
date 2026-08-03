/**
 * 墨水屏位图渲染模块
 *
 * 职责：把标准化车票信息渲染成 1bit 位图（每像素 0/1），输出 Buffer。
 *
 * 设计要点：
 *   - 使用 @napi-rs/canvas（预编译二进制，无需原生编译环境）
 *   - 1bit 输出：抗锯齿关闭，文字/线条按 1px 像素精确栅格化
 *   - 输出格式：MSB-first，行优先，每行按 8 像素对齐
 *     固件端 GxEPD2::drawBitmap() 期望的就是这个字节序
 *   - 分辨率：250 x 122（2.7 寸 SSD1680 横屏，通过 setRotation(1) 实现）
 *
 * 与 GxEPD2 1bit 缓冲区布局约定（严格 1bit 单色图，每行 8bit 对齐补齐）：
 *   - 字节内位序：MSB 在左（bit7 = 最左像素）
 *   - 行序：自顶向下
 *   - 每行字节数：ceil(W / 8) = 32 (250 / 8 = 32 字节 = 256 bit，
 *                                超出 250 像素的 6 bit 为 padding bit = 0)
 *   - 总字节数：32 * 122 = 3904
 *   - 严格 1bit：每个 bit 只可能是 0（白）或 1（黑），无灰度
 *   - 行 8bit 对齐：ROW_BYTES=27 已按 8 像素对齐，不足位补 0（白）
 *
 *   1=黑（墨水屏物理黑点），0=白（未刷新）
 */
const fs = require('fs');
const path = require('path');
const { createCanvas, GlobalFonts } = require('@napi-rs/canvas');

/* ============================================================
 * 常量：屏幕与布局（横屏 250 x 122）
 * ============================================================ */

/** 屏幕宽度（像素）—— 物理屏是 122x250 竖屏，setRotation(1) 后显示为 250x122 横屏 */
const SCREEN_W = 250;
/** 屏幕高度（像素） */
const SCREEN_H = 122;
/** 每行字节数（250 / 8 = 31.25，向上取整 32） */
const ROW_BYTES = Math.ceil(SCREEN_W / 8);  // 32
/** 1bit 位图总字节数 */
const BITMAP_SIZE = ROW_BYTES * SCREEN_H;  // 3904

/** 主题色：白底黑字
 *  line 用 #444444（lum 68 < 128）确保经阈值二值化后仍可见；
 *  原 #888888（lum 136）阈值后会变白消失 */
const THEME_LIGHT = { bg: '#ffffff', fg: '#000000', line: '#444444' };
/** 主题色：黑底白字（line #999999 阈值后变白，黑底上可见，无需改） */
const THEME_DARK  = { bg: '#000000', fg: '#ffffff', line: '#999999' };

/** 空车票默认值（测试用：MOCK_TICKET=true 时渲染此对象生成 PNG 预览） */
const DEFAULT_EMPTY_TICKET = {
  date: '',
  daysLeft: null,
  trainNo: 'HP365',
  fromStation: '任意门',
  toStation: '幸福湾',
  departTime: '',
  arriveTime: '',
  carriage: '01',
  seat: '01F',
  checkPosition: 'VIP',
  orderNo: ''
};

/* ============================================================
 * 字体加载（启动时执行一次）
 * ============================================================ */

/**
 * 把 wqy-microhei 字体注册到 @napi-rs/canvas
 * 优先从 config.FONT_PATH 读，读不到再回退到系统字体路径
 * @returns {string|null} 成功返回字体家族名（'WenQuanYi Micro Hei'），失败返回 null
 */
function loadFont() {
  const FAMILY = 'Source Han Sans SC Bold'; // Source Han Sans SC 中文字体/WenQuanYi Micro Hei

  // 跳过重复注册（同一进程多次调用 render 时只注册一次）
  if (GlobalFonts.has && GlobalFonts.has(FAMILY)) return FAMILY;

  const candidates = [
    // 相对路径下的assets
    path.resolve(__dirname, '', 'assets', 'fonts', 'SourceHanSansSC-Bold.otf')
  ].filter(Boolean);

  for (const p of candidates) {
    try {
      if (fs.existsSync(p)) {
        // @napi-rs/canvas 推荐用 registerFromPath 传路径，避免读 Buffer 的限制
        // .ttc 文件也支持，但需确保 .ttc 内含中文字符（wqy-microhei.ttc 是单字体集合）
        GlobalFonts.registerFromPath(p, FAMILY);
        console.log(`[render] 字体已加载: ${p}`);
        return FAMILY;
      }
    } catch (e) {
      console.warn(`[render] 字体加载失败 ${p}: ${e.message || e}`);
    }
  }

  console.error('========================================================');
  console.error('[render] ✗ 未找到任何中文字体！中文会显示为方块');
  console.error('[render]   解决方案: 把 wqy-microhei.ttc 放到 mail-fetcher/assets/fonts/');
  console.error('[render]   下载地址: https://github.com/anthonyfok/fonts-wqy-microhei/raw/master/wqy-microhei.ttc');
  console.error('========================================================');
  return null;
}

/* ============================================================
 * 绘制工具
 * ============================================================ */

/**
 * 在 canvas 上画文字
 * @param {CanvasRenderingContext2D} ctx
 * @param {string} text
 * @param {number} x 左上角 X
 * @param {number} y 顶部 Y
 * @param {string} font CSS font 字符串
 * @param {string} color
 */
function drawText(ctx, text, x, y, font, color) {
  ctx.fillStyle = color;
  ctx.font = font;
  ctx.textBaseline = 'top';
  ctx.fillText(text, x, y);
}

/**
 * 量文字宽度（提前 ctx.font 后调用）
 * @param {CanvasRenderingContext2D} ctx
 * @param {string} text
 * @param {string} [font] 可选：要切换到的 CSS font 字符串；
 *        不传则用 ctx 当前 font（必须保证调用前已设过正确字体）
 * @returns {number}
 */
function measure(ctx, text, font) {
  if (font) ctx.font = font;
  return ctx.measureText(text).width;
}

/**
 * 绘制 1px 水平线
 * @param {CanvasRenderingContext2D} ctx
 * @param {number} x1
 * @param {number} x2
 * @param {number} y
 * @param {string} color
 */
function drawHLine(ctx, x1, x2, y, color) {
  ctx.fillStyle = color;
  ctx.fillRect(x1, y, x2 - x1, 1);
}

/**
 * 截断站名（防止过长超出屏宽）
 * @param {string} name
 * @param {number} maxChars
 */
function truncStation(name, maxChars) {
  if (!name) return '';
  return name.length <= maxChars ? name : name.substring(0, maxChars);
}

/* ============================================================
 * 主渲染：车票（有票场景）—— 横屏 250x122
 * ============================================================
 * 布局（基于 design/ticket-layout-250x122.html）：
 *
 *   Y=22  [R1-L] 出发站     [R1-C] 车次   [R1-R] 到达站
 *   Y=33                 车次下划线 2px
 *   Y=45  [R2-L] 出发时间                [R2-R] 到达时间
 *   Y=55  ──────────── 分隔线 1px ────────────
 *   Y=63              [倒计时块 16px 高，今日=反色]
 *   Y=76  [R3] 中文日期(2026年7月6日 08:13开)    倒计时文字
 *   Y=90  ──────────── 分隔线 1px ────────────
 *   Y=110 [R4] 车厢座位(左)            检票口:1A(右)
 *
 * 方案 A 特点：无首条分隔线，站名+时间紧凑，下部留白加大
 */

/**
 * 渲染有票场景（方案A）
 * @param {object} ticket 标准化车票字段
 * @param {object} [opts]
 * @param {string} [opts.nowIso] 当前时间 ISO 字符串
 * @param {boolean} [opts.inverted] 是否反色
 * @returns {Buffer} 1bit 位图 Buffer
 */
function renderTicket(ticket, opts = {}) {
  const { inverted = false, nowIso = null } = opts;
  const showEmpty = !ticket || !ticket.date;
  if (showEmpty) {
    // 无票场景：使用默认空车票
    console.warn('[render] 无票场景，使用默认空车票');
    ticket = DEFAULT_EMPTY_TICKET;
  }
  const theme = inverted ? THEME_DARK : THEME_LIGHT;
  const family = loadFont();
  const FONT = family || '';

  const canvas = createCanvas(SCREEN_W, SCREEN_H);
  const ctx = canvas.getContext('2d', { alpha: false });

  // 关闭平滑插值（1bit 输出需像素精确栅格化）
  ctx.imageSmoothingEnabled = false;

  // 背景（无首条分隔线）
  ctx.fillStyle = theme.bg;
  ctx.fillRect(0, 0, SCREEN_W, SCREEN_H);

  /* 字体（统一最小 14px，适配更大屏幕） */
  const F_STATION = `bold 16px ${FONT}`;                       // 站名
  const F_TRAIN   = `bold 20px ${FONT}`;                       // 车次
  const F_TIME    = `15px ${FONT}`;                            // 时间
  const F_DETAIL  = `15px ${FONT}`;                            // 详情（日期）
  const F_CN_SM   = `15px ${FONT}`;                            // 中文小（座位/检票口）
  const F_CD      = `bold 15px ${FONT}`;                       // 倒计时

  const L = 6;
  const R = 244;
  const CX = 125;

  /* ===== R1: 站名+车次（无顶线，紧贴顶部）===== */
  const from = truncStation(ticket.fromStation || ticket.from || '', 3);
  const to   = truncStation(ticket.toStation   || ticket.to   || '', 3);
  drawText(ctx, from, L, 8, F_STATION, theme.fg);
  const toW = measure(ctx, to, F_STATION);
  drawText(ctx, to, R - toW, 8, F_STATION, theme.fg);

  const trainNo = ticket.trainNo || '';
  const trainW = measure(ctx, trainNo, F_TRAIN);
  drawText(ctx, trainNo, Math.floor(CX - trainW / 2), 6, F_TRAIN, theme.fg);
  // 车次下划线 2px（字体20px，下方留4px间距）
  const ulX1 = Math.floor(CX - trainW / 2) - 2;
  const ulX2 = Math.ceil(CX + trainW / 2) + 2;
  ctx.fillStyle = theme.fg;
  ctx.fillRect(ulX1, 30, ulX2 - ulX1, 2);

  /* ===== R2: 出发时间 — 到达时间 ===== */
  const depT = ticket.departTime || '--:--';
  const arrT = ticket.arriveTime || '--:--';
  drawText(ctx, depT, L, 32, F_TIME, theme.fg);
  const arrW = measure(ctx, arrT, F_TIME);
  drawText(ctx, arrT, R - arrW, 32, F_TIME, theme.fg);

  /* ===== 分隔线 Y=55 ===== */
  drawHLine(ctx, 5, 245, 55, theme.line);

  /* ===== R3: 中文日期 + 倒计时 ===== */
  let cnDate = '';
  let daysLeft = null;
  if (ticket.date) {
    const parts = ticket.date.split('-');
    if (parts.length === 3) {
      // 设计稿格式：2026年7月6日 08:13开（月份用阿拉伯数字）
      const m = parseInt(parts[1], 10);
      const d = parseInt(parts[2], 10);
      cnDate = `${parts[0]}年${m}月${d}日 ${depT}开`;
    }
    // 计算倒计时：按东八区日期差计算，避免 24 小时内发车的票都显示"今日"
    if (nowIso && depT) {
      const nowTs = new Date(nowIso).getTime();
      // 把 UTC 时间戳转成东八区日期字符串（中国无夏令时，固定 +8h）
      const nowDayStr = new Date(nowTs + 8 * 3600 * 1000).toISOString().slice(0, 10);
      daysLeft = Math.round((new Date(ticket.date) - new Date(nowDayStr)) / 86400000);
    } else if (ticket.daysLeft != null) {
      daysLeft = ticket.daysLeft;
    }
  }
  if (!cnDate && showEmpty) {
    cnDate = '随时随地';
    daysLeft = 0;
  }
  if (cnDate) {
    drawText(ctx, cnDate, 5, 65, F_DETAIL, theme.fg);
  }

  // 倒计时（右上角）
  if (daysLeft != null) {
    const isToday = daysLeft <= 0;
    // "今日" 两字 15px 等宽字间距为 0，加半角空格让两字分离避免抗锯齿粘连
    const countdownText = isToday ? '今日' : `${daysLeft}天后`;
    const cdW = measure(ctx, countdownText, F_CD);
    const cdX = R - cdW - 5;
    const cdY = 63;
    if (isToday) {
      // 今日：反色块强调（块高度 16 + 上下 padding 各 2）
      ctx.fillStyle = theme.fg;
      ctx.fillRect(cdX - 5, cdY, cdW + 10, 20);
      drawText(ctx, countdownText, cdX, cdY + 2, F_CD, theme.bg);
    } else {
      drawText(ctx, countdownText, cdX, cdY, F_CD, theme.fg);
    }
  }

  /* ===== 分隔线 Y=90 ===== */
  drawHLine(ctx, 5, 245, 90, theme.line);

  /* ===== R4: 座位 + 检票口 ===== */
  const seatText = `${ticket.carriage || ''}车${ticket.seat || ''}`;
  drawText(ctx, seatText, 5, 97, F_CN_SM, theme.fg);
  const gateText = ticket.checkPosition || ticket.gate || '--';
  const gateW = measure(ctx, gateText, F_CN_SM);
  drawText(ctx, gateText, R - gateW, 97, F_CN_SM, theme.fg);

  return canvasToBitmap(canvas);
}
/**
 * 把 canvas 像素数据转换成 1bit Buffer
 * 算法：每 8 个像素打包成 1 字节，MSB 在左
 *   阈值策略（按主题色自适应）：
 *     - 正常色（白底黑字，字=0 / 底=255，字边缘抗锯齿 128~200）：
 *         用阈值 200，让字边缘浅灰也判黑 → 小字完整不被"吃"
 *     - 反色（黑底白字，字=255 / 底=0，字边缘抗锯齿 55~127）：
 *         用阈值 128，让字边缘深灰也判白 → 保持字形原粗细，不被"削"
 *   - 1bit 墨水屏标准做法，既保证小字清晰又保证边缘锐利
 *
 * 输出契约（严格 1bit 单色图 + 每行 8bit 对齐补齐）：
 *   - 每个像素占 1 bit，bit 值 ∈ {0, 1}，无灰度
 *   - 每行 32 字节（250 像素 + 6 bit padding = 0）
 *   - 缓冲区总大小固定 3904 字节
 *   - padding bit（每行末 6 bit）显式写 0，保证非零脏数据不会泄漏
 * @param {Canvas} canvas
 * @param {object} [opts]
 * @param {boolean} [opts.inverted] 是否反色模式；true→阈值 128，false→阈值 200
 * @returns {Buffer}
 */
function canvasToBitmap(canvas, opts = {}) {
  const { inverted = false } = opts;
  const threshold = inverted ? 150 : 200;
  // 同一个 canvas 重复 getContext('2d') 会返回同一实例，保留 fillStyle/font 等设置
  const ctx = canvas.getContext('2d');
  const imgData = ctx.getImageData(0, 0, SCREEN_W, SCREEN_H);

  const { data } = imgData;

  // 严格 1bit 输出 + 行 8bit 对齐：Buffer.alloc 默认填 0，每行末尾 padding bit 天然为 0
  const out = Buffer.alloc(BITMAP_SIZE, 0);
  for (let y = 0; y < SCREEN_H; y++) {
    const rowBase = y * ROW_BYTES;
    for (let x = 0; x < SCREEN_W; x++) {
      const i = (y * SCREEN_W + x) * 4;
      // 取绿色通道作为亮度近似
      const lum = data[i + 1];
      const bit = lum < threshold ? 1 : 0;
      const byteIdx = rowBase + (x >> 3);
      const bitIdx = 7 - (x & 7);  // MSB-first
      if (bit) out[byteIdx] |= (1 << bitIdx);
    }
    // 防御性补齐：每行末位 250~255 共 6 bit 显式清零，
    // 防止未来重构时因非零脏数据被 OR 写入导致 padding bit ≠ 0
    out[rowBase + (SCREEN_W >> 3)] &= 0xc0;  // 32 字节最后一字节保留高 2 bit
  }

  return out;
}

/* ============================================================
 * 调试：把 1bit Buffer 转成 PNG 保存
 * ============================================================ */

/**
 * 把 1bit Buffer 转成 PNG Buffer
 * @param {Buffer} bitmap 1bit 位图
 * @returns {Buffer} PNG Buffer
 */
function bitmapToPng(bitmap) {
  const canvas = createCanvas(SCREEN_W, SCREEN_H);
  const ctx = canvas.getContext('2d', { alpha: false });
  const imgData = ctx.createImageData(SCREEN_W, SCREEN_H);
  for (let y = 0; y < SCREEN_H; y++) {
    for (let x = 0; x < SCREEN_W; x++) {
      const byteIdx = y * ROW_BYTES + (x >> 3);
      const bitIdx = 7 - (x & 7);
      const isBlack = (bitmap[byteIdx] >> bitIdx) & 1;
      const i = (y * SCREEN_W + x) * 4;
      const v = isBlack ? 0 : 255;
      imgData.data[i + 0] = v;
      imgData.data[i + 1] = v;
      imgData.data[i + 2] = v;
      imgData.data[i + 3] = 255;
    }
  }
  ctx.putImageData(imgData, 0, 0);
  return canvas.toBuffer('image/png');
}

/**
 * 渲染无票场景（兼容 test-render.js 调用）
 * 实际 /ticket 路由已不使用，由 renderTicket(null) 内部回退到 DEFAULT_EMPTY_TICKET
 */
function renderEmpty(nowIso, opts = {}) {
  return renderTicket(DEFAULT_EMPTY_TICKET, { ...opts, nowIso });
}

/**
 * 1bit 位图最近邻缩放
 *
 * 用于服务端将 250×122 标准渲染缩放到设备实际屏幕尺寸（如 212×104）。
 * 设备通过 X-Screen-W / X-Screen-H header 上报尺寸，服务端据此缩放。
 *
 * @param {Buffer} src  源位图（MSB-first, 行优先, 每行 8bit 对齐）
 * @param {number} srcW 源宽度（像素）
 * @param {number} srcH 源高度（像素）
 * @param {number} dstW 目标宽度（像素）
 * @param {number} dstH 目标高度（像素）
 * @returns {Buffer} 缩放后位图（尺寸 = ceil(dstW/8) * dstH）
 */
function scale1bitBitmap(src, srcW, srcH, dstW, dstH) {
  if (srcW === dstW && srcH === dstH) return src;
  const srcRow = Math.ceil(srcW / 8);
  const dstRow = Math.ceil(dstW / 8);
  const dst = Buffer.alloc(dstRow * dstH);
  for (let y = 0; y < dstH; y++) {
    const sy = Math.floor(y * srcH / dstH);
    for (let x = 0; x < dstW; x++) {
      const sx = Math.floor(x * srcW / dstW);
      const srcByte = sy * srcRow + (sx >> 3);
      const srcBit = 7 - (sx & 7);
      if ((src[srcByte] >> srcBit) & 1) {
        const dstByte = y * dstRow + (x >> 3);
        const dstBit = 7 - (x & 7);
        dst[dstByte] |= (1 << dstBit);
      }
    }
  }
  return dst;
}

module.exports = {
  SCREEN_W,
  SCREEN_H,
  ROW_BYTES,
  BITMAP_SIZE,
  renderTicket,
  renderEmpty,
  bitmapToPng,
  loadFont,
  scale1bitBitmap,
  DEFAULT_EMPTY_TICKET,
};
