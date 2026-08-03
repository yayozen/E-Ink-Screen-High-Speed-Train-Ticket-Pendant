/**
 * 渲染测试脚本（不连 IMAP）
 * 用 mock 车票数据生成位图 + PNG 预览，便于人工对照渲染效果
 *
 * 用法：
 *   node src/test-render.js
 *
 * 输出：
 *   tmp-previews/*.png
 */
const fs = require('fs');
const path = require('path');
const { renderTicket, renderEmpty, bitmapToPng, BITMAP_SIZE } = require('./render-bitmap');

const OUT_DIR = path.resolve(__dirname, '..', 'tmp-previews');
if (!fs.existsSync(OUT_DIR)) fs.mkdirSync(OUT_DIR, { recursive: true });

/**
 * 把 Buffer 写到 PNG 文件
 */
function savePng(name, bitmap) {
  const png = bitmapToPng(bitmap);
  const file = path.join(OUT_DIR, name);
  fs.writeFileSync(file, png);
  console.log(`✓ ${name} (${bitmap.length} 字节位图, ${png.length} 字节 PNG)`);
}

/**
 * Mock 测试用例
 */
const nowIso = '2026-07-09T08:00:00+08:00';

const tickets = [
  {
    name: 'ticket-今天.png',
    ticket: {
      date: '2026-07-09',
      trainNo: 'G7178',
      fromStation: '合肥南',
      toStation: '上海虹桥',
      departTime: '08:13',
      arriveTime: '10:13',
      carriage: '05',
      seat: '07F',
      checkPosition: '检票口1A',
      orderNo: 'EE123456',
    },
    inverted: false,
  },
  {
    name: 'ticket-3天后.png',
    ticket: {
      date: '2026-07-12',
      trainNo: 'G1234',
      fromStation: '北京南',
      toStation: '济南西',
      departTime: '14:30',
      arriveTime: '18:45',
      carriage: '03',
      seat: '12A',
      checkPosition: '检票口8B',
      orderNo: 'EE234567',
    },
    inverted: false,
  },
  {
    name: 'ticket-长站名.png',
    ticket: {
      date: '2026-07-15',
      trainNo: 'G1826',
      fromStation: '上海虹桥',
      toStation: '合肥南',
      departTime: '18:14',
      arriveTime: '21:30',
      carriage: '11',
      seat: '15F',
      checkPosition: '检票口7A7B',
      orderNo: 'EE345678',
    },
    inverted: false,
  },
  {
    name: 'ticket-今天-反色.png',
    ticket: {
      date: '2026-07-09',
      trainNo: 'G7178',
      fromStation: '合肥南',
      toStation: '上海虹桥',
      departTime: '08:13',
      arriveTime: '10:13',
      carriage: '05',
      seat: '07F',
      checkPosition: '检票口1A',
      orderNo: 'EE123456',
    },
    inverted: true,
  },
  {
    name: 'ticket-3天后-反色.png',
    ticket: {
      date: '2026-07-12',
      trainNo: 'G1234',
      fromStation: '北京南',
      toStation: '济南西',
      departTime: '14:30',
      arriveTime: '18:45',
      carriage: '03',
      seat: '12A',
      checkPosition: '检票口8B',
      orderNo: 'EE234567',
    },
    inverted: true,
  },
];

console.log(`\n=== 渲染测试（屏幕 ${104}x${212}） ===\n`);

// 1. 有票场景
for (const t of tickets) {
  const bitmap = renderTicket(t.ticket, { nowIso, inverted: t.inverted });
  savePng(t.name, bitmap);
}

// 2. 无票场景
const emptyBitmap = renderEmpty(nowIso);
savePng('empty.png', emptyBitmap);

const emptyInv = renderEmpty(nowIso, { inverted: true });
savePng('empty-inv.png', emptyInv);

console.log(`\n共生成 ${tickets.length + 2} 张 PNG，目录: ${OUT_DIR}`);
console.log(`位图大小: ${BITMAP_SIZE} 字节 (${104}*${212}/8 = ${104*212/8})`);
console.log('人工目测 PNG 即可验证布局/字体大小/对齐是否符合预期\n');
