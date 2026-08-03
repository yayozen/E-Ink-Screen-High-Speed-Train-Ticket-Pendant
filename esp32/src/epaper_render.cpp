/**
 * 2.13 寸墨水屏渲染实现（黑白红三色 / GDEY0213Z98 / SSD1680Z 控制器）
 *
 * 关键点：
 *   - GxEPD2_3C<GxEPD2_213_Z98c, GxEPD2_213_Z98c::HEIGHT>
 *     物理 128x250 竖屏，可见区域 122x250；
 *     setRotation(1) 后逻辑坐标 250x122 横屏
 *   - page_height 取 HEIGHT (250)，一次性写完 1 帧，最简单
 *   - 当前只使用黑色图层（drawBitmap + GxEPD_BLACK），
 *     红色 buffer 由 fillScreen(GxEPD_WHITE) 保持全 0xFF（=无红点），
 *     屏幕呈现纯黑白效果
 *   - 全刷通过 display.firstPage()/nextPage() 内部循环完成
 */
#include "epaper_render.h"
#include "config.h"
#include <Arduino.h>
#include <SPI.h>
#include <mbedtls/base64.h>
/* GxEPD2 附带 Adafruit GFX 字体库，用于 ASCII 文本显示（调试用） */
#include <Fonts/FreeSansBold12pt7b.h>

#if defined(RWB_SCREEN)
#include <GxEPD2_3C.h>
#include <epd3c/GxEPD2_213_Z98c.h>
/* 2.13 寸 SSD1680Z 三色屏实例（物理 128x250，可见 122x250）
 * 注：虽然 GxEPD2_3C 支持黑白红三色，本工程目前仅使用黑色图层 */
static GxEPD2_3C<GxEPD2_213_Z98c, GxEPD2_213_Z98c::HEIGHT> display(
  GxEPD2_213_Z98c(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);
#else
#include <GxEPD2_BW.h>
// 2.13寸黑白屏
static GxEPD2_BW<GxEPD2_213_BN, GxEPD2_213_BN::HEIGHT> display(
  GxEPD2_213_BN(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);
#endif
#if defined(SCREEN_212)
static const uint16_t offsetY = 18;
#else
static const uint16_t offsetY = 0;
#endif

/* 屏幕连接状态：epaperInit() 探测成功后置 true；未连接时所有显示/休眠方法直接跳过
 * 防止后续 firstPage/nextPage/hibernate 因 SPI 无应答而卡死 */
static bool screenReady = false;

/**
 * 在当前 page buffer 内绘制一段支持 '\n' 换行的 ASCII 文本
 * 调用方须确保：
 *   1) 已选好字体（display.setFont）—— 当前统一用 FreeSansBold12pt7b
 *   2) 已选好颜色（display.setTextColor）—— 当前统一用 GxEPD_BLACK
 *   3) 当前已在 firstPage/nextPage 循环的某一页内
 * 行间距取字体的 yAdvance；GxEPD2 的 display.print(const char*) 不识别 '\n'，
 * 故必须逐字符遍历手动处理
 */
static void drawTextMultiline(const char *text, int16_t x, int16_t y) {
  if (!text || !*text) return;

  int16_t lineHeight = FreeSansBold12pt7b.yAdvance || 12;

  int16_t cursorY = y + offsetY;
  display.setCursor(x, cursorY);

  const char *lineStart = text; // 当前行起始指针
  const char *p = text;        // 遍历指针

  while (*p) {
    if (*p == '\n') {
      // 打印当前行（从 lineStart 到 p，不含换行符）
      size_t lineLen = p - lineStart;
      display.write((const uint8_t *)lineStart, lineLen);

      // 换行
      cursorY += lineHeight;
      display.setCursor(x, cursorY);

      lineStart = p + 1; // 跳到下一行起始
    }
    p++;
  }

  // 打印最后一行（文本无末尾换行的情况）
  if (lineStart < p) {
    display.write((const uint8_t *)lineStart, p - lineStart);
  }
}

/**
 * 初始化墨水屏 + SPI 总线（带首屏全刷）
 * 旋转 1：将 128x250 物理屏旋转为 250x122 横屏显示（与设计稿对齐）
 *
 * 连接检测策略：
 *   - 屏幕无 MISO（SPI 单向写入），无法回读控制器寄存器，只能借 BUSY 引脚判定
 *   - 先做一次硬件复位（RST 拉低→拉高），等待 BUSY 拉低
 *   - 正常屏幕 BUSY 会在 ~200ms 内拉低；超过 2s 仍未拉低视为未连接
 *   - 探测失败：screenReady=false，后续显示/休眠方法直接 return 不阻塞
 *   - 探测成功：再走 GxEPD2 完整 init + 全刷清屏流程
 *
 * 幂等性：已成功初始化后再次调用直接 return，避免重复全刷（每次全刷 ~25s）
 *
 * @param bootText  非空时，在首屏清屏的同时把该文本画到屏幕（同一帧 buffer，
 *                  不再触发额外一次全刷）；为空则只清屏。推荐用于"BLE 配网中..."/
 *                  "WiFi 连接失败"等需要立即给用户反馈的场景
 * @param bootX     文本 baseline 起点 x（仅在 bootText 非空时生效）
 * @param bootY     文本 baseline 起点 y（仅在 bootText 非空时生效）
 */
void epaperInit(const char *bootText = nullptr, int16_t bootX = 0, int16_t bootY = 30, const uint8_t *bitmap = nullptr) {
  // 幂等保护：已初始化则直接 return（避免重复全刷再等 25s）
  if (screenReady) {
    Serial.println("[EPAD] 屏幕已初始化，跳过");
    if (bootText && *bootText) {
      epaperDrawText(bootText, bootX, bootY);
    }
    return;
  }
  // 打印spi
  Serial.printf("[EPAD] SPI 总线引脚: SCK=%d, MOSI=%d, CS=%d\n", EPD_SCK, EPD_MOSI, EPD_CS);

  /* 1) 这里 screenReady 必然 false，可以直接走 init 流程） */
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);

  // pinMode(EPD_RST, OUTPUT);
  // digitalWrite(EPD_RST, HIGH);
  // delay(30);
  // digitalWrite(EPD_RST, LOW);
  // delay(30);
  // digitalWrite(EPD_RST, HIGH);
  // delay(50);

  display.init(115200, true, 2, false);
  display.setRotation(1);
  display.setFont(&FreeSansBold12pt7b);
  display.setTextColor(GxEPD_BLACK);
  screenReady = true;
  Serial.println("[EPAD] 控制器 init 完成");

  /* 2) 全刷清屏（带 bootText 走"同一帧 buffer"路径） */
  display.setFullWindow();
  display.firstPage();
  do {
    // fillScreen(WHITE) 同时把 _black_buffer 和 _color_buffer 置 0xFF（全白/无红点）
    display.fillScreen(GxEPD_WHITE);

    // 若有启动文本，预先设好字体/颜色（这些设置在 firstPage/nextPage 之外是 page 间持久的）
    if (bootText && *bootText && !bitmap) {
      Serial.printf("[EPAD] 首屏将显示文本: \"%s\" @(%d,%d)\n", bootText, bootX, bootY);
      drawTextMultiline(bootText, bootX, bootY);
    }

    if (bitmap) {
      Serial.println("[EPAD] 首屏将显示位图");
      display.drawBitmap(0, offsetY, bitmap, EPD_WIDTH, EPD_HEIGHT, GxEPD_BLACK);
    }

  } while (display.nextPage());

  Serial.println("[EPAD] 屏幕初始化完成");
}

/**
 * 把 1bit 位图（仅黑白色）刷到屏幕上
 * @param bitmap 1bit 数据，长度 = EPD_BITMAP_SIZE
 * @param len   字节数
 * @return true 成功，false 参数非法
 *
 * 实现思路：
 *   走 display 的 firstPage/nextPage 模式，内部循环按 page_height 切片写入控制器。
 *   每一页先 fillScreen(GxEPD_WHITE) 复位黑/红 buffer，再用 drawBitmap 按逻辑坐标
 *   把 1bit 数据写到黑色图层（bit=1 落黑点）。
 *   红色 buffer 在 fillScreen 之后保持 0xFF（=无红点），所以屏幕不显示红色。
 *   display.nextPage() 内部会触发 _Update_Full 完成全屏刷新（约 25s）。
 */
bool epaperDrawBitmap(const uint8_t *bitmap, size_t len) {
  if (!bitmap || len != EPD_BITMAP_SIZE) {
    Serial.printf("[EPAD] 位图长度非法: %u, 期望 %u\n", (unsigned)len, (unsigned)EPD_BITMAP_SIZE);
    return false;
  }

  // 屏幕未就绪：跳过刷屏（防止 nextPage 在 SPI 无应答时死循环）
  if (!screenReady) {
    Serial.println("[EPAD] 屏幕未就绪，先初始化控制器");
    epaperInit(nullptr, 0, 30, bitmap);
    return true; 
  }

  Serial.println("[EPAD] 开始刷屏（黑白，全刷约 25s）");
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    // 仅写黑色图层：1bit 数据，bit=1 落黑点
    // drawBitmap(x, y, bitmap, w, h, color) —— 参数顺序与 Adafruit_GFX 一致
    display.drawBitmap(0, offsetY, bitmap, EPD_WIDTH, EPD_HEIGHT, GxEPD_BLACK);
  } while (display.nextPage());
  Serial.println("[EPAD] 刷屏完成");
  return true;
}

/**
 * 在墨水屏上显示 ASCII 文本（用于调试日志 / 错误提示）
 * @param text  ASCII 字符串指针（仅支持 0x20~0x7E 可见 ASCII；含 '\n' 时按字体 yAdvance 换行）
 * @param x     baseline 起点 x 坐标（屏幕逻辑坐标 0..EPD_WIDTH-1）
 * @param y     第一行 baseline y 坐标（0..EPD_HEIGHT-1；建议预留 18px 给字高）
 *
 * 实现思路：
 *   - 字体选 FreeSansBold 12pt（GxEPD2 自带 Adafruit GFX 字库，零额外 flash 占用）
 *   - 行高取字体的 yAdvance（FreeSansBold12pt7b.yAdvance），'\n' 时按此值下移光标
 *   - 走 firstPage/nextPage 全刷模式；每页 fillScreen(WHITE) 后按字符输出
 *   - 颜色固定为 GxEPD_BLACK（红色 buffer 维持 0xFF=无红点，与位图路径保持一致）
 *   - 非 ASCII 字符会被 GFX 跳过（不报错），符合"调试用"定位
 *   - 全刷约 25s，期间不可断电
 */
void epaperDrawText(const char *text, int16_t x, int16_t y) {

  // 空文本：直接返回（避免空指针异常）
  if (!text || !*text) {
    Serial.println("[EPAD] epaperDrawText: 空文本");
    return;
  }

  // 屏幕未就绪：跳过刷屏（防止 firstPage/nextPage 卡死）
  if (!screenReady) {
    Serial.println("[EPAD] 屏幕未就绪，跳过文本显示");
    epaperInit(text, x, y, nullptr);
    return;
  }

  Serial.printf("[EPAD] 显示文本: \"%s\" @(%d,%d)\n", text, x, y);
  display.setFont(&FreeSansBold12pt7b);
  display.setTextColor(GxEPD_BLACK);

  // 走 firstPage/nextPage 全刷；每页内通过 drawTextMultiline 完成
  // 逐字符 + '\n' 换行的绘制（行间距取字体 yAdvance）
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawTextMultiline(text, x, y);
  } while (display.nextPage());
  Serial.println("[EPAD] 文本显示完成");
}

/**
 * 让墨水屏进入休眠
 * 调用后墨水屏会进入低功耗模式（不再响应 SPI，但画面保持）
 *
 * GxEPD2_213_Z98c::hibernate() 内部链路：
 *   _PowerOff() → _waitWhileBusy(power_off_time)  // 已等 BUSY 拉低
 *   → 发 0x10 deep sleep 命令 → _hibernating=true
 * 故外层无需再 delay；原先的 delay(50) 是冗余的保守兜底
 */
void epaperHibernate() {
  // 屏幕未就绪：直接跳过（hibernate 内部会拉 SPI，未连屏可能阻塞）
  if (!screenReady) return;
  display.hibernate();
}
