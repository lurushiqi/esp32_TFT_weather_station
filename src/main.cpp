#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <TFT_eSPI.h>
#include <OneButton.h>
#include <time.h>

#include "kaijitu.h"      // 开机全屏图 128x160
#include "temp.h"         // 温度 32x32
#include "shidu.h"        // 湿度 32x32
#include "CO2.h"          // CO2  32x32
#include "rain.h"         // 雨量 32x32
#include "cloud.h"        // 气压 32x32
#include "alt.h"          // 海拔 32x32
#include "air.h"          // 空气质量 32x32
#include "tvoc.h"         // TVOC 32x32
#include "msyh16.h"       // 16px 中文字库（数值 + 标签 + 星期，全用它）

// ============================== 用 户 配 置 ==============================
const char* WIFI_SSID     = "iQOO";
const char* WIFI_PASSWORD = "llllffff";

const char* MQTT_SERVER   = "broker.emqx.io";
const int   MQTT_PORT     = 1883;

// ============================== MQTT 主 题 ==============================
// 必须与检测端（发布方）完全一致。检测端用 MQTT_TOPIC_BASE 统一前缀，默认 "weather/"，
// 4 个主题会同步变化
//    weather/temp_hum  → "温度,湿度"
//    weather/press_alt → "气压,海拔"
//    weather/air       → "CO2,TVOC,空气质量"
//    weather/rain      → "雨量ADC,雨量等级"
const char* MQTT_TOPIC_BASE = "weather/";
String      topicTempHum;     // 温湿度
String      topicPressAlt;    // 气压海拔
String      topicAir;         // 空气：CO2 + TVOC + 空气质量
String      topicRain;        // 雨量

// ============================== 硬 件 定 义 ==============================
#define TFT_ROTATION   0        // 竖屏 128(宽) x 160(高)
#define BUTTON_PIN     14
#define TFT_BL_PIN     -1       // 背光引脚，不需要就保持 -1

TFT_eSPI  tft = TFT_eSPI();
OneButton button(BUTTON_PIN, true);   // true = 按下接 GND，启用内部上拉

WiFiClient   espClient;
PubSubClient mqtt(espClient);

// ============================== 颜 色 定 义 ==============================
#define C_BG     tft.color565(243, 246, 250)
#define C_TEXT   tft.color565( 44,  62,  80)
#define C_MUTED  tft.color565(156, 163, 175)
#define C_LINE   tft.color565(229, 231, 235)
#define C_BAR    tft.color565( 51,  65,  85)
#define C_RED    tft.color565(239,  68,  68)
#define C_GREEN  tft.color565( 16, 185, 129)
#define C_ORANGE tft.color565(245, 158,  11)
#define C_BLUE   tft.color565( 59, 130, 246)
#define C_PURPLE tft.color565(139,  92, 246)
#define C_SLATE  tft.color565(100, 116, 139)
#define C_LIME   tft.color565(132, 204,  22)
// MQTT 断开时用的灰色
#define C_OFFLINE tft.color565(170, 176, 185)

// ============================== 版 面 常 量 ==============================
#define TOPBAR_H  18
#define CARD_H    32
#define CARD_GAP   3
#define CARD_X     2
#define CARD_W   124
#define LIST_Y0   (TOPBAR_H + 1)      // 19
#define DOT_Y    157

#define ICON_X      (CARD_X + 4)      // 图标左边界
#define ICON_Y_OFF   2                // 图标相对卡片顶部偏移
#define TEXT_X      (CARD_X + 38)     // 文字左边界
#define LABEL_Y_OFF  1                // 标签（16px，字身高约15，占1~16）
#define VALUE_Y_OFF 16                // 数值（16px，占16~31）

// ============================== 数 据 结 构 ==============================
struct WeatherData {
  float temp    = NAN;      // 温度 ℃
  float humi    = NAN;      // 湿度 %
  int   co2     = -1;       // eCO2 ppm
  int   tvoc    = -1;       // TVOC ppb
  float press   = NAN;      // 气压 hPa
  float alt     = NAN;      // 海拔 m
  int   rainAdc = -1;       // 雨量 ADC（值越小雨越大）
  char  rainLevel[12] = ""; // 无雨 / 小雨 / 中雨 / 大雨
  char  airLevel[12]  = ""; // 优 / 良 / 中 / 差 / 传感器离线
  bool  valid   = false;    // 收到过数据吗（收到过就一直留着）
};
WeatherData d;              // 全局保留：断网重连后不会丢

// ===== 检测端数据新鲜度（检测端断电/掉线后自动变 "--"，防止旧数据误导） =====
// 检测端每 5 秒发一次数据（READ_INTERVAL=5000），这里 15 秒（3 个周期）没收到
// 就判定检测端掉线：所有卡片数值变 "--"。注意 mqttOK 只管"MQTT 连着没"——
// 检测端断电时显示端连的是公网 broker，MQTT 其实还是通的，必须靠数据新鲜度区分。
#define DATA_TIMEOUT_MS 15000
unsigned long lastDataMs  = 0;     // 最后一次收到检测端数据的时间（millis）
bool          dataFresh   = false; // 数据是否新鲜（收到过数据且未超时）

uint8_t page = 0;
bool    needRedraw = true;
bool    needRefresh = false;   // 数据更新只重画数值，不整屏重画（局部刷新）

bool          mqttOK      = false;
unsigned long mqttBackoff = 3000;
unsigned long lastMqttTry = 0;

// ============================== 函 数 声 明 ==============================
void drawPage1();
void drawPage2();
void drawTopBar();
void drawPageDots(uint8_t cur);
void drawNoNetwork();
void drawReconnectBar(uint8_t n);
void drawWelcome();
void connectMQTT();
void onMqttMessage(char* topic, byte* payload, unsigned int len);
int  splitCsv(char* s, char* out[], int maxN);
uint16_t rainColor();
uint16_t airColor();
const char* airText();
void drawLabel(const char* s, int x, int y, uint16_t bg);
void drawValue(const String& s, int x, int y, uint16_t color, uint16_t bg);
void drawListCard(const uint16_t* icon, const char* label,
                  const String& value, const char* unit,
                  uint8_t idx, int y, uint16_t valColor);

// ============================== 文 字 工 具 ==============================
// msyh16 是比例字体，宽度必须用 textWidth() 实测，不能估
//
// ★★★ 必须用 setTextColor(前景, 背景) 两个参数 ★★★
// 平滑字体(vlw)是带抗锯齿灰度的，TFT_eSPI 画的时候要拿"背景色"去混合
// 每个字的边缘像素。只传前景色会沿用上一次的背景色（开机默认是 0x0000 黑），
// 于是每个字周围都会被糊上一圈深灰描边 —— 在浅色卡片底上看起来就是
// "字体模糊 + 重影"，白字画在深色顶栏上反而不明显，所以之前一直没发现。
// 结论：只要 loadFont 画字，就必须显式给出背景色，且要和字底下真实的底色一致。

void drawLabel(const char* s, int x, int y, uint16_t bg) {
  tft.loadFont(msyh16);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_MUTED, bg);
  tft.setCursor(x, y);
  tft.print(s);
  tft.unloadFont();
}

void drawValue(const String& s, int x, int y, uint16_t color, uint16_t bg) {
  tft.loadFont(msyh16);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color, bg);
  tft.setCursor(x, y);
  tft.print(s);
  tft.unloadFont();
}

// ============================== 卡 片 ==============================
uint16_t cardPlate(uint8_t idx) {
  switch (idx) {
    case 0: return tft.color565(254, 242, 242);   // 温度 浅红
    case 1: return tft.color565(255, 251, 235);   // 湿度 浅黄
    case 2: return tft.color565(240, 253, 244);   // CO2  浅绿
    case 3: return tft.color565(239, 246, 255);   // 雨量 浅蓝
    case 4: return tft.color565(248, 250, 252);   // 气压 浅灰
    case 5: return tft.color565(245, 243, 255);   // 海拔 浅紫
    case 6: return tft.color565(236, 253, 245);   // 空气 浅绿
    default:return tft.color565(255, 247, 237);   // TVOC 浅橙
  }
}

//
//  一行列表卡（卡高 32）：
//    +------------------------------------------------+
//    | [图标28]  标签（12px，灰）                       | y+3
//    |           数值（16px，彩色，带单位）             | y+15
//    +------------------------------------------------+
//  注意：不再有"灰色/无数据"状态，图标和配色永远是正常的
//
void drawListCard(const uint16_t* icon, const char* label,
                  const String& value, const char* unit,
                  uint8_t idx, int y, uint16_t valColor) {
  // MQTT 断了，或检测端超过 15 秒没发数据（断电/掉线），就把卡片变灰、数值显示 "--"
  bool off = (!mqttOK || !dataFresh);
  uint16_t vc = off ? C_OFFLINE : valColor;
  String   vs = off ? "--" : value;

  // 卡片底
  const uint16_t plate = cardPlate(idx);
  tft.fillRoundRect(CARD_X, y, CARD_W, CARD_H, 4, plate);
  // 底部细色条（两侧内缩，收在圆角内）
  tft.fillRoundRect(CARD_X + 6, y + CARD_H - 3, CARD_W - 12, 2, 1, vc);

  // 图标：宽高必须和图标数组的真实尺寸一致（现在是 28x28）
  // 之前这里写 28x28 但数组是 32x32，pushImage 不会缩放、只按 28 宽顺序读数据，
  // 导致每行错位 4 个像素、越往下越错，就是看到的彩色噪点花屏。
  // 最后一个参数 0x0000 是透明色：跳过图标周围的空白，不会糊成黑方块。
  // 实测确认：图片工具导出的数组是"字节已交换"格式，必须用 setSwapBytes(false)。
  // 之前默认写 true，颜色会整体错乱。开机图和 8 个图标统一用 false。
  tft.setSwapBytes(false);
  tft.pushImage(ICON_X, y + ICON_Y_OFF, 28, 28, (const uint16_t*)icon, 0x0000);

  // 文字背景色必须传卡片底色，否则抗锯齿边缘会被糊上一圈黑灰描边
  drawLabel(label, TEXT_X, y + LABEL_Y_OFF, plate);

  String v = vs;
  if (!off && unit && unit[0]) v += unit;   // 掉线时不带单位，只显示 "--"
  drawValue(v, TEXT_X, y + VALUE_Y_OFF, vc, plate);
}

// ============================== 顶 栏 ==============================
// 星期名称。struct tm 的 tm_wday：0=周日、1=周一 … 6=周六（跟我们习惯的顺序不同）
// 这几个字在 msyh16 里都有，缺字会画成空白，改动文案前先确认字库。
const char* WEEK_NAME[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

void drawTopBar() {
  tft.fillRect(0, 0, 128, TOPBAR_H, C_BAR);

  struct tm ti;
  bool ok = getLocalTime(&ti, 0);      // NTP 没同步成功时为 false，下面都按"--"处理

  // ① 左：时钟（含秒；顶栏每秒重画一次，正好配合秒的跳动）
  //    字库上沿 13。数字 dY=12、字高 12，光标 y=2 时数字占 3~15，在 18px 顶栏里居中
  char b[12] = "--:--:--";
  if (ok) sprintf(b, "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
  tft.loadFont(msyh16);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, C_BAR);        // ★ 背景色必须给，否则字边糊黑圈
  tft.setCursor(4, 2);
  tft.print(b);
  tft.unloadFont();

  // ② 中：星期（加了秒之后时钟变宽，星期改右对齐避让，右缘停在 116，给状态灯留位）
  //    汉字 dY=13、字高 14，光标 y=2 时字身正好占 2~16
  const char* w = ok ? WEEK_NAME[ti.tm_wday] : "--";
  tft.loadFont(msyh16);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, C_BAR);        // ★ 同上
  tft.setCursor(116 - tft.textWidth(w), 2);
  tft.print(w);
  tft.unloadFont();

  // ③ 右：网络状态灯  绿=MQTT 通且数据新鲜 / 橙=MQTT 通但检测端掉线（超时无数据）/ 红=WiFi 断
  uint16_t nc = (WiFi.status() != WL_CONNECTED) ? C_RED
                : (mqttOK ? (dataFresh ? C_GREEN : C_ORANGE) : C_ORANGE);
  tft.fillCircle(122, 9, 3, nc);  // 视觉上跟文字中线对齐
}

// ============================== 页 码 点 ==============================
void drawPageDots(uint8_t cur) {
  tft.fillCircle(58, DOT_Y, 2, cur == 0 ? C_TEXT : C_LINE);
  tft.fillCircle(70, DOT_Y, 2, cur == 1 ? C_TEXT : C_LINE);
}

// ============================== 雨 量 / 空 气 配 色 ==============================
// 检测端 rainLevelFromAdc() 的分级：
//   >3500 无雨 / >2500 小雨 / >1500 中雨 / 其余 大雨
// 先按文字匹配，匹配不上再退回用 ADC 判断，双保险
uint16_t rainColor() {
  if (strstr(d.rainLevel, "大雨")) return tft.color565( 30,  64, 175);
  if (strstr(d.rainLevel, "中雨")) return tft.color565( 37,  99, 235);
  if (strstr(d.rainLevel, "小雨")) return tft.color565( 96, 165, 250);
  if (d.rainAdc > 0) {
    if (d.rainAdc <= 1500) return tft.color565( 30,  64, 175);
    if (d.rainAdc <= 2500) return tft.color565( 37,  99, 235);
    if (d.rainAdc <= 3500) return tft.color565( 96, 165, 250);
  }
  return C_BLUE;
}

uint16_t airColor() {
  if (strstr(d.airLevel, "优")) return C_GREEN;
  if (strstr(d.airLevel, "良")) return C_LIME;
  if (strstr(d.airLevel, "中")) return C_ORANGE;
  if (strstr(d.airLevel, "差")) return C_RED;
  return C_SLATE;                       // "传感器离线" 等异常
}

// 空气质量文字：检测端 SGP30 离线时会发"传感器离线"，
// 5 个字在 128px 屏上太挤，这里只显示"离线"两个字
const char* airText() {
  if (!d.valid || d.airLevel[0] == '\0') return "--";
  if (strstr(d.airLevel, "优")) return "优";
  if (strstr(d.airLevel, "良")) return "良";
  if (strstr(d.airLevel, "中")) return "中";
  if (strstr(d.airLevel, "差")) return "差";
  return "离线";
}

// ============================== 局 部 刷 新 ==============================
// 数据更新时只擦写"数值 + 色条"，图标/标签/卡片底都不动，
// 整屏骨架只在翻页/断线时画一次 → 不再整页闪烁
void refreshCardValue(const String& value, const char* unit,
                      uint8_t idx, int y, uint16_t valColor) {
  bool off = (!mqttOK || !dataFresh);   // MQTT 断 或 检测端掉线 → 显示 "--"
  uint16_t vc = off ? C_OFFLINE : valColor;
  String   vs = off ? "--" : value;

  const uint16_t plate = cardPlate(idx);

  // 擦掉旧数值（数值区：TEXT_X ~ 卡片右缘留 4px，高 16，宽 82px，覆盖所有数值长度）
  tft.fillRect(TEXT_X, y + VALUE_Y_OFF, CARD_X + CARD_W - 4 - TEXT_X, 16, plate);
  // 底部细色条（跟 drawListCard 一致）
  tft.fillRoundRect(CARD_X + 6, y + CARD_H - 3, CARD_W - 12, 2, 1, vc);

  // 画新数值（掉线不带单位，只显示 "--"）
  String v = vs;
  if (!off && unit && unit[0]) v += unit;
  drawValue(v, TEXT_X, y + VALUE_Y_OFF, vc, plate);
}

void refreshPageValues(uint8_t pg) {
  int y = LIST_Y0;
  const int step = CARD_H + CARD_GAP;

  if (pg == 0) {
    bool t = d.valid && !isnan(d.temp);
    refreshCardValue(t ? String(d.temp, 1) : "--", t ? "℃" : "", 0, y, C_RED); y += step;
    bool h = d.valid && !isnan(d.humi);
    refreshCardValue(h ? String(d.humi, 0) : "--", h ? "%" : "", 1, y, C_ORANGE); y += step;
    bool c = d.valid && d.co2 >= 0;
    refreshCardValue(c ? String(d.co2) : "--", c ? "ppm" : "", 2, y, C_GREEN); y += step;
    bool r = d.valid && d.rainAdc >= 0 && d.rainLevel[0];
    refreshCardValue(r ? String(d.rainLevel) : "--", "", 3, y, rainColor());
  } else {
    bool p = d.valid && !isnan(d.press);
    refreshCardValue(p ? String(d.press, 1) : "--", p ? "hPa" : "", 4, y, C_SLATE); y += step;
    bool a = d.valid && !isnan(d.alt);
    refreshCardValue(a ? String(d.alt, 1) : "--", a ? "m" : "", 5, y, C_PURPLE); y += step;
    refreshCardValue(airText(), "", 6, y, airColor()); y += step;
    bool v = d.valid && d.tvoc >= 0;
    refreshCardValue(v ? String(d.tvoc) : "--", v ? "ppb" : "", 7, y, C_ORANGE);
  }
}

// ============================== 第 1 页 ==============================
void drawPage1() {
  tft.fillScreen(C_BG);
  drawTopBar();

  int y = LIST_Y0;
  const int step = CARD_H + CARD_GAP;

  // 温度：没收到过数据显示 "--"，收到过就一直显示最后一次的值
  bool t = d.valid && !isnan(d.temp);
  drawListCard(temp, "温度", t ? String(d.temp, 1) : "--", t ? "℃" : "",
               0, y, C_RED);
  y += step;

  // 湿度
  bool h = d.valid && !isnan(d.humi);
  drawListCard(shidu, "湿度", h ? String(d.humi, 0) : "--", h ? "%" : "",
               1, y, C_ORANGE);
  y += step;

  // CO2
  bool c = d.valid && d.co2 >= 0;
  drawListCard(CO2, "CO2", c ? String(d.co2) : "--", c ? "ppm" : "",
               2, y, C_GREEN);
  y += step;

  // 雨量（等级文字：无雨 / 小雨 / 中雨 / 大雨）
  bool r = d.valid && d.rainAdc >= 0 && d.rainLevel[0];
  drawListCard(rain, "雨量", r ? String(d.rainLevel) : "--", "",
               3, y, rainColor());

  drawPageDots(0);
}

// ============================== 第 2 页 ==============================
void drawPage2() {
  tft.fillScreen(C_BG);
  drawTopBar();

  int y = LIST_Y0;
  const int step = CARD_H + CARD_GAP;

  // 气压
  bool p = d.valid && !isnan(d.press);
  drawListCard(cloud, "气压", p ? String(d.press, 1) : "--", p ? "hPa" : "",
               4, y, C_SLATE);
  y += step;

  // 海拔
  bool a = d.valid && !isnan(d.alt);
  drawListCard(alt, "海拔", a ? String(d.alt, 1) : "--", a ? "m" : "",
               5, y, C_PURPLE);
  y += step;

  // 空气质量
  drawListCard(air, "空气质量", airText(), "", 6, y, airColor());
  y += step;

  // TVOC
  bool v = d.valid && d.tvoc >= 0;
  drawListCard(tvoc, "TVOC", v ? String(d.tvoc) : "--", v ? "ppb" : "",
               7, y, C_ORANGE);

  drawPageDots(1);
}

// ============================== 断 网 页 ==============================
// B 版：浅红光晕 + 实心红圆 + 白色感叹号，文字全部排在图标下方，
//       底部一条"正在重连"胶囊。整页不带顶栏（断网时没有时间也没有网络灯可显示）。
//       之前 A 版把白字压在白色斜杠上，128x160 上糊成一团，已弃用。
void drawNoNetwork() {
  tft.fillScreen(C_BG);

  const int cx = 64, cy = 46;

  // ① 浅红光晕 + 实心红圆（直径 60 / 52，屏宽 128 上比例舒服）
  tft.fillCircle(cx, cy, 30, tft.color565(254, 226, 226));
  tft.fillCircle(cx, cy, 26, C_RED);

  // ② 白色感叹号：竖条 5x21 + 圆点直径 7
  tft.fillRoundRect(62, 28, 5, 21, 2, TFT_WHITE);
  tft.fillCircle(64, 56, 3, TFT_WHITE);

  // ③ 主标题：字库上沿 13、汉字 dY=13，光标 y 就是字身顶边，这里占 80~94
  tft.loadFont(msyh16);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_TEXT, C_BG);            // ★ 底色 C_BG
  tft.setCursor(64 - tft.textWidth("未连接") / 2, 82);
  tft.print("未连接");
  tft.unloadFont();

  // ④ 副标题：占 102~116
  tft.loadFont(msyh16);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_MUTED, C_BG);           // ★ 底色 C_BG
  tft.setCursor(64 - tft.textWidth("等待网络连接") / 2, 102);
  tft.print("等待网络连接");
  tft.unloadFont();

  drawReconnectBar(0);
}

// 底部"正在重连"胶囊。n = 后面跟几个点（0~3），由 loop 每 800ms 推进，
// 让人一眼看出程序还活着、正在重试，而不是死机白屏。
void drawReconnectBar(uint8_t n) {
  tft.fillRoundRect(10, 132, 108, 22, 11, C_LINE);

  tft.loadFont(msyh16);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_SLATE, C_LINE);         // ★ 底色是胶囊的 C_LINE
  // 左边界按"满点数"算一次并固定住，这样点数变化时文字不会左右抖
  int x0 = 64 - tft.textWidth("正在重连···") / 2;
  // 占 136~150
  tft.setCursor(x0, 136);
  tft.print("正在重连");
  for (uint8_t i = 0; i < n; i++) tft.print("·");
  tft.unloadFont();
}

// ============================== 开 机 页（原样保留，只显示 kaijitu） ==============================
void drawWelcome() {
  // kaijitu.h 是图片工具导出的"字节已交换"格式，用 false 才对（自检实测确认）。
  // 另外它的数组类型是 int16_t，pushImage 只接受 uint16_t*，所以要强制转换；
  // 16 位色值的二进制内容完全一样，转换不影响任何像素，以后重新导出也不用改这里。
  tft.setSwapBytes(false);
  tft.pushImage(0, 0, 128, 160, (const uint16_t*)kaijitu);
}

// ============================== MQTT ==============================
// 按逗号切分字符串，最多 maxN 段，返回实际段数
int splitCsv(char* s, char* out[], int maxN) {
  int n = 0;
  out[n++] = s;
  while (*s && n < maxN) {
    if (*s == ',') { *s = '\0'; out[n++] = s + 1; }
    s++;
  }
  return n;
}

// 回调：检测端把数据拆成 4 个主题分别发布，这里按"主题名"路由，
// 只更新对应字段，其余字段保留上一次的值（d 是全局变量，断网也不丢）。
void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  if (len == 0 || len > 200) return;               // 长度护栏

  char buf[201];
  memcpy(buf, payload, len);
  buf[len] = '\0';

  char* f[8];
  int n = splitCsv(buf, f, 8);

  if (strcmp(topicTempHum.c_str(),  topic) == 0) {        // 温湿度："温度,湿度"
    if (n < 2) return;
    float t = atof(f[0]);
    float h = atof(f[1]);
    if (t < -40.0f || t > 80.0f) return;                  // 合法性检查，防乱码
    if (h <  0.0f || h > 100.0f) return;
    d.temp = t; d.humi = h;
    d.valid = true;
  }
  else if (strcmp(topicPressAlt.c_str(), topic) == 0) {    // 气压海拔："气压,海拔"
    if (n < 2) return;
    d.press = atof(f[0]);
    d.alt   = atof(f[1]);
    d.valid = true;
  }
  else if (strcmp(topicAir.c_str(), topic) == 0) {         // 空气："CO2,TVOC,空气质量"
    if (n < 3) return;
    d.co2  = atoi(f[0]);
    d.tvoc = atoi(f[1]);
    strncpy(d.airLevel, f[2], sizeof(d.airLevel) - 1);
    d.airLevel[sizeof(d.airLevel) - 1] = '\0';
    d.valid = true;
  }
  else if (strcmp(topicRain.c_str(), topic) == 0) {        // 雨量："雨量ADC,雨量等级"
    if (n < 2) return;
    d.rainAdc = atoi(f[0]);
    strncpy(d.rainLevel, f[1], sizeof(d.rainLevel) - 1);
    d.rainLevel[sizeof(d.rainLevel) - 1] = '\0';
    d.valid = true;
  }
  else {
    return;                                                // 未知主题，丢弃
  }

  // 记下"最后收到数据"的时间：检测端掉线超时判断用
  lastDataMs = millis();
  dataFresh  = true;
  needRefresh = true;           // 回调里只置标志，不画屏（数据更新走局部刷新）
}

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttTry < mqttBackoff) return;
  lastMqttTry = millis();

  uint64_t mac = ESP.getEfuseMac();
  char cid[32];
  sprintf(cid, "weather_disp_%04X", (uint16_t)(mac & 0xFFFF));

  if (mqtt.connect(cid)) {
    // 订阅检测端的 4 个分主题（与发布端一一对应）
    mqtt.subscribe(topicTempHum.c_str());
    mqtt.subscribe(topicPressAlt.c_str());
    mqtt.subscribe(topicAir.c_str());
    mqtt.subscribe(topicRain.c_str());
    mqttOK = true;
    mqttBackoff = 3000;
  } else {
    mqttOK = false;
    mqttBackoff = min(mqttBackoff * 2, 30000UL);   // 3s→6s→12s→24s→30s
  }
}

// ============================== 按 键 ==============================
void onButtonClick() {
  page = (page + 1) % 2;
  needRedraw = true;
}

// ============================== setup ==============================
void setup() {
  Serial.begin(115200);

#if TFT_BL_PIN >= 0
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);
#endif

  tft.init();
  tft.setRotation(TFT_ROTATION);
  tft.fillScreen(C_BG);

  button.attachClick(onButtonClick);
  button.setDebounceTicks(30);
  button.setClickTicks(400);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  configTime(8 * 3600, 0, "ntp.aliyun.com", "cn.pool.ntp.org");

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(256);
  mqtt.setKeepAlive(30);   // 30 秒没收到服务器回应就判定断线，屏幕随即变灰

  // 拼出 4 个 MQTT 订阅主题（前缀与检测端 MQTT_TOPIC_BASE 保持一致）
  topicTempHum  = String(MQTT_TOPIC_BASE) + "temp_hum";
  topicPressAlt = String(MQTT_TOPIC_BASE) + "press_alt";
  topicAir      = String(MQTT_TOPIC_BASE) + "air";
  topicRain     = String(MQTT_TOPIC_BASE) + "rain";

  // 开机页 2.5 秒，期间后台继续连网
  drawWelcome();
  unsigned long t0 = millis();
  while (millis() - t0 < 2500) {
    button.tick();
    delay(10);
  }

  needRedraw = true;
}

// ============================== loop ==============================
unsigned long lastStatusMs = 0;

void loop() {
  button.tick();

  if (WiFi.status() == WL_CONNECTED) {
    connectMQTT();
    mqtt.loop();
  } else {
    mqttOK = false;
    static unsigned long lastWifiTry = 0;
    if (millis() - lastWifiTry > 5000) {
      lastWifiTry = millis();
      WiFi.reconnect();
    }
  }

  // 断网页 ⇄ 主页面
  // 断网期间 d 里的数据一直留着，网一回来重画就是"上次的数据"
  static bool showingNoNet = false;
  if (WiFi.status() != WL_CONNECTED) {
    if (!showingNoNet) { showingNoNet = true; drawNoNetwork(); }
    // 让"正在重连"后面的点 0→1→2→3 循环，证明程序还在跑
    if (millis() - lastStatusMs > 800) {
      lastStatusMs = millis();
      static uint8_t dotN = 0;
      dotN = (dotN + 1) % 4;
      drawReconnectBar(dotN);
    }
    return;
  } else if (showingNoNet) {
    showingNoNet = false;
    needRedraw = true;
  }

  // MQTT 一通一断，立刻重画屏幕（变灰或恢复原样）
  static bool lastMqttOK = false;
  if (mqttOK != lastMqttOK) {
    lastMqttOK = mqttOK;
    needRedraw = true;
  }

  // 每秒整体重画一次顶栏（时钟 + 星期 + 状态灯）
  // 原来只擦时钟和灯那一小块，加了中间的星期之后局部重画容易互相覆盖，
  // 直接整条重画最省心，128x16 的开销可以忽略。
  if (millis() - lastStatusMs > 1000) {
    lastStatusMs = millis();
    drawTopBar();
  }

  // 检测端掉线检测：15 秒没收到数据 → dataFresh 置 false，
  // 触发局部刷新把数值全部变 "--"（断电后不残留旧数据误导人）
  if (dataFresh && millis() - lastDataMs > DATA_TIMEOUT_MS) {
    dataFresh = false;
    needRefresh = true;
  }

  // 数据更新（MQTT 消息）：只局部刷新当前页数值，不整屏重画 → 不闪
  // 断网页显示中就先不刷（数据照常收，联网后重画就是最新值）
  if (needRefresh) {
    needRefresh = false;
    if (WiFi.status() == WL_CONNECTED && !showingNoNet) refreshPageValues(page);
  }

  // 整屏重画：只在翻页 / 断网恢复 / MQTT 状态变化时发生（次数很少，不会闪成片）
  if (needRedraw) {
    needRedraw = false;
    if (page == 0) drawPage1(); else drawPage2();
  }

  delay(20);
}
