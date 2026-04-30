#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <OneButton.h>
#include <PubSubClient.h>

// ====================== 配置项 ======================
const char* WIFI_SSID = "iQOO";
const char* WIFI_PWD = "llllffff";
const uint8_t TFT_ROTATION = 0;

// MQTT
const char* MQTT_BROKER = "broker.emqx.io";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_SUB_TOPIC = "weather_station_data";

#include "msyh16.h"
#include "kaijitu.h"
#include "temp.h"
#include "shidu.h"
#include "CO2.h"

// ====================== 硬件对象 ======================
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 60*60*8, 30*60*1000);
TFT_eSPI tft;
OneButton btn(14, true, true); // 按键 GPIO14

WiFiClient espClient;
PubSubClient mqtt(espClient);

// ====================== 函数声明 ======================
void drawArrayJpeg(const uint8_t arrayname[], uint32_t array_size, int xpos, int ypos);
void renderJPEG(int xpos, int ypos);
void showStaticScreen1();
void showPage2();
void updatePage2Value();
void updateDynamicData();
void reconnect_mqtt();
void mqtt_callback(char* topic, byte* payload, unsigned int length);
void showNoNetworkScreen();
void forceRefreshAllData();
void drawTopBar();
void changePage();

#define minimum(a,b) (((a) < (b)) ? (a) : (b))

// ====================== 全局数据 ======================
float temperature = 25.0;
float humidity = 50.0;
int air_quality = 300;
int co2 = 400;
int tvoc = 0;
float pressure = 1013.0;
float altitude = 0.0;

float temp_new = 25.0;
float humi_new = 50.0;
int air_new = 300;
int co2_new = 400;
int tvoc_new = 0;
float press_new = 1013.0;
float alt_new = 0.0;

bool mqtt_need_update = false;
int page = 1;

float old_temp = -999;
float old_humi = -999;
int old_air = -999;
int old_co2 = -999;
int old_tvoc = -999;
float old_press = -999;
float old_alt = -999;

bool lastWifiConnected = false;

// ====================== setup ======================
void setup()
{
  Serial.begin(115200);
  tft.init();
  tft.setRotation(TFT_ROTATION);
  tft.fillScreen(TFT_WHITE);

  tft.pushImage(0, 0, KAIJITU_WIDTH, KAIJITU_HEIGHT, (uint16_t*)kaijitu);
  delay(2000);

  btn.attachClick(changePage);

  WiFi.begin(WIFI_SSID, WIFI_PWD);
  delay(1000);

  if (WiFi.status() != WL_CONNECTED) {
    showNoNetworkScreen();
  }

  timeClient.begin();
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqtt_callback);

  lastWifiConnected = (WiFi.status() == WL_CONNECTED);
  if (lastWifiConnected) {
    showStaticScreen1();
    forceRefreshAllData();
  }
}

// ====================== loop ======================
void loop()
{
  btn.tick();
  bool wifiOk = (WiFi.status() == WL_CONNECTED);

  if (wifiOk && !lastWifiConnected) {
    tft.fillScreen(TFT_WHITE);
    page = 1;
    showStaticScreen1();
    forceRefreshAllData();
    lastWifiConnected = true;
  }

  if (!wifiOk && lastWifiConnected) {
    showNoNetworkScreen();
    lastWifiConnected = false;
  }

  if (wifiOk) {
    reconnect_mqtt();
    mqtt.loop();
    timeClient.update();

    if (mqtt_need_update) {
      temperature = temp_new;
      humidity = humi_new;
      air_quality = air_new;
      co2 = co2_new;
      tvoc = tvoc_new;
      pressure = press_new;
      altitude = alt_new;
      mqtt_need_update = false;
    }

    updateDynamicData();
  }

  delay(50);
}

// ====================== MQTT 重连 ======================
void reconnect_mqtt() {
  if (!mqtt.connected()) {
    char clientId[30];
    snprintf(clientId, 30, "ESP32_DISPLAY_%d", random(1000,9999));
    if (mqtt.connect(clientId)) {
      mqtt.subscribe(MQTT_SUB_TOPIC);
    }
  }
}

// ====================== MQTT 回调 ======================
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  char data[150];
  for (int i=0; i<length; i++) data[i]=(char)payload[i];
  data[length]=0;

  sscanf(data,"%f,%f,%d,%d,%d,%f,%f",
    &temp_new,&humi_new,&air_new,&co2_new,&tvoc_new,&press_new,&alt_new);

  mqtt_need_update = true;
}

// ====================== 按键切换 —— 修复：不白屏 ======================
void changePage() {
  page = (page == 1) ? 2 : 1;

  // 🔥 修复：刷屏时临时关闭中断，防止WiFi抢线
  noInterrupts();
  if (page == 1) {
    showStaticScreen1();
  } else {
    showPage2();
  }
  interrupts();

  forceRefreshAllData();
}

void forceRefreshAllData() {
  old_temp = old_humi = old_air = old_co2 = old_tvoc = old_press = old_alt = -999;
  updateDynamicData();
}

// ====================== 顶部时间 ======================
void drawTopBar() {
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.fillRect(5, 5, 120, 20, TFT_WHITE);
  tft.setCursor(5,5);
  tft.print(timeClient.getFormattedTime());

  tft.setTextSize(1);
  tft.setTextColor(TFT_GREEN, TFT_WHITE);
  tft.fillRect(105,8,30,16, TFT_WHITE);
  tft.setCursor(105,8);

  int wd = timeClient.getDay();
  const char* wk[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
  tft.print(wk[wd]);
  tft.drawFastHLine(0,25,128,TFT_RED);
}

// ====================== 页面1 ======================
void showStaticScreen1() {
  tft.fillScreen(TFT_WHITE);
  drawTopBar();

  for (int y=0; y<32; y++) {
    for (int x=0; x<32; x++) {
      uint16_t c = temp[y*32+x];
      c = (c>>8)|(c<<8);
      if (c) tft.drawPixel(10+x, 38+y, c);
    }
  }
  for (int y=0; y<32; y++) {
    for (int x=0; x<32; x++) {
      uint16_t c = shidu[y*32+x];
      c = (c>>8)|(c<<8);
      if (c) tft.drawPixel(10+x, 80+y, c);
    }
  }
  for (int y=0; y<32; y++) {
    for (int x=0; x<32; x++) {
      uint16_t c = CO2[y*32+x];
      c = (c>>8)|(c<<8);
      if (c) tft.drawPixel(10+x, 122+y, c);
    }
  }
}

// ====================== 页面2 —— 红线画到底，字体不重影 ======================
void showPage2()
{
  tft.fillScreen(TFT_WHITE);
  drawTopBar();

  tft.drawFastHLine(0, 90, 128, TFT_BLUE);
  tft.drawFastVLine(64, 25, 135, TFT_BLUE); 

  tft.loadFont(msyh16);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);

  tft.setCursor(14, 35);  tft.println("气压");
  tft.setCursor(16, 50);  tft.println("hPa");
  tft.setCursor(14+64+2, 35); tft.println("海拔");
  tft.setCursor(16+64+8, 50); tft.println("m");
  tft.setCursor(0, 35+65); tft.println("空气质量");
  tft.setCursor(14+64, 35+65); tft.println("TVOC");
  tft.setCursor(14+64+2, 35+65+15);tft.println("ppb");
  tft.unloadFont();
}

// ====================== 页面2 数值更新 ======================
void updatePage2Value()
{
  static unsigned long lastTime = 0;
  if (millis() - lastTime > 1000) {
    drawTopBar();
    lastTime = millis();
  }

  tft.loadFont(msyh16);
  tft.setTextColor(TFT_BLACK);

  if (pressure != old_press) {
    tft.fillRect(0, 66, 60, 20, TFT_WHITE);
    tft.setCursor(10, 66); tft.printf("%.0f", pressure);
    old_press = pressure;
  }
  if (altitude != old_alt) {
    tft.fillRect(64+10, 66, 60, 20, TFT_WHITE);
    tft.setCursor(64+10, 66); tft.printf("%.1f", altitude);
    old_alt = altitude;
  }
  if (air_quality != old_air) {
    tft.fillRect(0, 66+65+2, 60, 20, TFT_WHITE);
  tft.setCursor(15, 66+65+2);

  if (air_quality >= 0 && air_quality <= 2500) {
    tft.print("优");
  } else if (air_quality >= 2501 && air_quality <= 3000) {
    tft.print("良");
  } else if (air_quality >= 3001 && air_quality <= 3500) {
    tft.print("中");
  } else if (air_quality >= 3501 && air_quality <= 4000) {
    tft.print("差");
  } else {
    tft.print("严重");
  }

    old_air = air_quality;
  }
  if (tvoc != old_tvoc) {
    tft.fillRect(64+10, 66+65+2, 60, 20, TFT_WHITE);
    tft.setCursor(64+15, 66+65+2); tft.printf("%d", tvoc);
    old_tvoc = tvoc;
  }
  tft.unloadFont();
}

// ====================== 主刷新 ======================
void updateDynamicData() {
  if (page == 1) {
    static unsigned long lastTimePage1 = 0;
    if (millis() - lastTimePage1 > 1000) {
      drawTopBar();
      lastTimePage1 = millis();
    }
    tft.loadFont(msyh16);

    if (temperature != old_temp) {
      tft.fillRect(50,42,70,16, TFT_WHITE);
       tft.setTextColor(TFT_BLACK);
      tft.setCursor(50,42); tft.print(temperature,1); tft.print("℃");
      old_temp = temperature;
    }
    if (humidity != old_humi) {
      tft.fillRect(50,82,70,16, TFT_WHITE);
       tft.setTextColor(TFT_BLACK);
      tft.setCursor(50,82); tft.print(humidity,1); tft.print("%");
      old_humi = humidity;
    }
    if (co2 != old_co2) {
      tft.fillRect(50,126,70,16, TFT_WHITE);
       tft.setTextColor(TFT_BLACK);
      tft.setCursor(50,126); tft.print(co2);
      old_co2 = co2;
    }
    tft.unloadFont();
  }

  if (page == 2) {
    updatePage2Value();
  }
}

// ====================== 断网页面 ======================
void showNoNetworkScreen() {
  tft.fillScreen(TFT_WHITE);
  int cx=64, cy=50;
  tft.drawCircle(cx,cy,22,TFT_RED);
  tft.drawLine(cx-12,cy-12,cx+12,cy+12,TFT_RED);
  tft.drawLine(cx+12,cy-12,cx-12,cy+12,TFT_RED);

  tft.loadFont(msyh16);
  tft.setTextColor(TFT_RED,TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("未连接到网络",64,100);
  tft.unloadFont();
}