/**
 * 2.13 寸墨水屏渲染（黑白红三色 / GDEY0213Z98 / SSD1680Z 控制器）
 *
 * 当前绘制策略：仅使用黑色图层，红色 buffer 保持 0xFF（全白/无红点），
 *              屏幕上呈现纯黑白效果（与原 1bit 单色屏一致）。
 *
 * 新方案：固件端只负责把服务端生成的 1bit 位图灌到屏幕，
 *        不再做任何字体渲染/中文支持/坐标计算。
 *
 * 位图格式约定（与服务端 render-bitmap.js 一致）：
 *   - 1 bit per pixel，MSB-first，行优先
 *   - 每行字节数 = ceil(W/8) = 32   （250 像素 / 8 = 32 字节，超出 6 bit padding = 0）
 *   - 总字节数 = 32 * 122 = 3904
 *   - bit=1 落黑点，bit=0 白点
 *
 * 屏幕逻辑分辨率：setRotation(1) 后 250 x 122（物理 128x250 竖屏旋转）
 */
#pragma once

#include <Arduino.h>

/* 屏幕分辨率（与服务端一致） */
#define EPD_BITMAP_ROW_BYTES  ((EPD_WIDTH + 7) / 8)          // 32
#define EPD_BITMAP_SIZE       (EPD_BITMAP_ROW_BYTES * EPD_HEIGHT)  // 3904

/**
 * 把 1bit 位图（仅黑白色）刷到墨水屏
 * @param bitmap 1bit 数据指针，长度必须 = EPD_BITMAP_SIZE
 * @param len    数据长度（防御性检查，传 0 或不匹配则直接 return）
 * @return true 刷屏成功，false 参数非法
 *
 * 注意：刷屏会触发全刷（~25s），期间请勿断电
 */
bool epaperDrawBitmap(const uint8_t *bitmap, size_t len);

/**
 * 在墨水屏上显示一行 ASCII 文本（用于调试日志 / 错误提示）
 * @param text  ASCII 字符串指针（仅支持 0x20~0x7E 可见 ASCII，中文/非 ASCII 会被忽略）
 * @param x     文本左下角起始 x 坐标（屏幕逻辑坐标，0..EPD_WIDTH-1）
 * @param y     baseline y 坐标（0..EPD_HEIGHT-1；建议预留 18px 给字高）
 *
 * 实现：使用 GxEPD2 内置的 Adafruit GFX 字体（FreeSans Bold 12pt），
 *       走 firstPage/nextPage 全刷模式，每页先 fillScreen(WHITE) 清屏再 print 文本。
 * 注意：full refresh 约 25s，期间不可断电；调用前应已 epaperInit()
 *       副作用：会清空屏幕当前内容（全刷）
 */
void epaperDrawText(const char *text, int16_t x, int16_t y);

/**
 * 让墨水屏进入休眠（节省功耗）
 * 刷完屏必调，否则持续耗电
 */
void epaperHibernate();
