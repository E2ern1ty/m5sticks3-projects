/*
 * Simple Watch — M5StickS3 极简手表/桌钟
 *
 * 开机自动连 WiFi 对时（NTP，东八区），然后：
 *   大数字 时:分（秒的冒号闪烁）
 *   日期 + 中文星期
 *   电量 + WiFi 状态点
 * A 键：亮度循环 100 / 50 / 15（夜间档）
 * B 键：重新对时（连 WiFi + NTP）
 */

#include <M5Unified.h>
#include <WiFi.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

static bool wifiOk = false;
static int brightIdx = 0;
static const uint8_t BRIGHTS[] = {100, 50, 15};
static uint32_t lastDrawMs = 0;
static uint32_t lastSyncMs = 0;
static const char *WEEK[] = {"日", "一", "二", "三", "四", "五", "六"};

static void syncTime(bool verbose) {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) M5.delay(100);
  }
  wifiOk = (WiFi.status() == WL_CONNECTED);
  if (wifiOk) {
    configTime(8 * 3600, 0, "ntp1.aliyun.com", "ntp2.aliyun.com", "pool.ntp.org");
    struct tm t;
    wifiOk = getLocalTime(&t, 8000);
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF); // 对完时关 WiFi 省电（RTC 继续走时）
    if (verbose) {
      M5.Lcd.setFont(&fonts::efontCN_12);
      M5.Lcd.setTextColor(wifiOk ? TFT_GREEN : TFT_RED, TFT_BLACK);
      M5.Lcd.fillRect(60, 110, 140, 18, TFT_BLACK);
      M5.Lcd.setCursor(60, 110);
      M5.Lcd.print(wifiOk ? "对时成功" : "对时失败");
      delay(1000);
    }
  } else if (verbose) {
    M5.Lcd.setFont(&fonts::efontCN_12);
    M5.Lcd.setTextColor(TFT_RED, TFT_BLACK);
    M5.Lcd.fillRect(60, 110, 140, 18, TFT_BLACK);
    M5.Lcd.setCursor(60, 110);
    M5.Lcd.print("WiFi 未连上");
    delay(1000);
  }
  lastSyncMs = millis();
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Lcd.setRotation(3);
  M5.Lcd.setBrightness(BRIGHTS[0]);
  M5.Lcd.fillScreen(TFT_BLACK);
  Serial.begin(115200);
  Serial.printf("\r\n=== Simple Watch ===\r\n");

  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Lcd.setCursor(70, 55);
  M5.Lcd.print("对时中 ...");
  syncTime(false);
}

void loop() {
  M5.update();

  if (M5.BtnA.wasClicked()) {
    brightIdx = (brightIdx + 1) % 3;
    M5.Lcd.setBrightness(BRIGHTS[brightIdx]);
  }
  // B 长按：休眠（RTC 继续走时，唤醒即恢复）
  if (M5.BtnB.pressedFor(1200)) {
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setFont(&fonts::efontCN_12);
    M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Lcd.setCursor(96, 60);
    M5.Lcd.print("休眠");
    delay(400);
    M5.Lcd.sleep();
    esp_sleep_enable_ext1_wakeup((1ULL << 11) | (1ULL << 12),
                                 ESP_EXT1_WAKEUP_ANY_LOW);
    M5.Power.deepSleep(m5::Power_Class::sleep_no_timer);
    return;
  }
  if (M5.BtnB.wasClicked()) {
    M5.Lcd.setFont(&fonts::efontCN_12);
    M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    M5.Lcd.fillRect(60, 110, 140, 18, TFT_BLACK);
    M5.Lcd.setCursor(76, 110);
    M5.Lcd.print("重新对时...");
    syncTime(true);
  }

  if (millis() - lastDrawMs > 250) {
    lastDrawMs = millis() - 249; // 立即触发
    lastDrawMs = millis();
    struct tm t;
    if (!getLocalTime(&t, 50)) {
      // 没有时间（从未对时成功）：显示占位
      M5.Lcd.setFont(&fonts::efontCN_12);
      M5.Lcd.setTextColor(TFT_RED, TFT_BLACK);
      M5.Lcd.setCursor(80, 50);
      M5.Lcd.print("时间未同步");
      M5.Lcd.setCursor(52, 72);
      M5.Lcd.print("连 WiFi 后按 B 对时");
      return;
    }
    bool colon = (t.tm_sec % 2 == 0);
    M5.Lcd.setFont(&fonts::Font7);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.fillRect(10, 30, 220, 56, TFT_BLACK);
    M5.Lcd.setCursor(16, 30);
    M5.Lcd.printf("%02d", t.tm_hour);
    M5.Lcd.setCursor(100, 30);
    M5.Lcd.print(colon ? ":" : " ");
    M5.Lcd.setCursor(150, 30);
    M5.Lcd.printf("%02d", t.tm_min);

    M5.Lcd.setFont(&fonts::efontCN_12);
    M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    M5.Lcd.fillRect(30, 90, 180, 18, TFT_BLACK);
    M5.Lcd.setCursor(30, 90);
    M5.Lcd.printf("%d月%d日 星期%s", t.tm_mon + 1, t.tm_mday, WEEK[t.tm_wday]);

    // 秒（小字，右侧）
    M5.Lcd.fillRect(212, 44, 28, 20, TFT_BLACK);
    M5.Lcd.setTextFont(2);
    M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Lcd.setCursor(214, 44);
    M5.Lcd.printf("%02d", t.tm_sec);

    // WiFi 状态点 + 电量
    M5.Lcd.fillCircle(232, 10, 3, wifiOk ? TFT_DARKGREEN : TFT_DARKGREY);
    int bat = M5.Power.getBatteryLevel();
    if (bat > 0) {
      M5.Lcd.fillRect(180, 6, 44, 10, TFT_BLACK);
      M5.Lcd.drawRect(180, 6, 40, 10, TFT_DARKGREY);
      M5.Lcd.fillRect(220, 8, 3, 6, TFT_DARKGREY);
      uint16_t bc = bat <= 20 ? TFT_RED : TFT_GREEN;
      M5.Lcd.fillRect(182, 8, (36 * bat) / 100, 6, bc);
    }
  }

  // 每 6 小时自动静默校时
  if (millis() - lastSyncMs > 6UL * 3600 * 1000) syncTime(false);

  delay(20);
}
