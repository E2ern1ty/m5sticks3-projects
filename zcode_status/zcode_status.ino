/*
 * ZCode Dashboard — M5StickS3 工作状态同步配件
 *
 * 轮询 Mac 上的 zcode_daemon.py（读 ~/.zcode 本地数据库），显示：
 *   - 当前会话任务简写（标题）
 *   - Todo 进度条 + 正在做的条目
 *   - 状态灯：绿=干活中 / 黄闪=等你接入 / 红=出错 / 灰=无会话
 *   - Token 消耗：累计总量 / 缓存命中 / 今日输出
 *
 * 按键：A 亮度三档；B 立即刷新；B 长按休眠
 * 依赖：Mac 端运行 tools/zcode_daemon.py（局域网 8765 端口）
 */

#include <M5Unified.h>
#include <utility/Power_Class.hpp>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

static const uint32_t POLL_MS = 3000;

// 状态快照
static String stTitle = "";
static String stProject = "";
static int stDone = 0, stTotal = 0;
static String stCurrent = "";
static String stState = "boot"; // running/idle/error/none/boot/conn
static uint32_t stAgo = 0;
static uint64_t tokAll = 0, tokCache = 0, tokDayOut = 0;
static int planWin = 0, planQuota = 600;
static uint64_t tokMonth = 0;
static bool blinkOn = true;

static int brightIdx = 0;
static const uint8_t BRIGHTS[] = {100, 50, 15};

static String fmtTok(uint64_t t) {
  char b[16];
  if (t >= 1000000000ULL) snprintf(b, sizeof(b), "%.2fB", t / 1e9);
  else if (t >= 1000000ULL) snprintf(b, sizeof(b), "%.1fM", t / 1e6);
  else if (t >= 1000ULL) snprintf(b, sizeof(b), "%lluK", (unsigned long long)(t / 1000));
  else snprintf(b, sizeof(b), "%llu", (unsigned long long)t);
  return String(b);
}

// UTF-8 折行（两行以内）
static void drawWrapped(const char *utf8, int x, int y, int maxW, int lines,
                        uint16_t color, int lineH, const void *f) {
  M5.Lcd.setFont((const lgfx::v1::IFont *)f);
  M5.Lcd.setTextColor(color, TFT_BLACK);
  int cx = x, line = 0;
  const unsigned char *p = (const unsigned char *)utf8;
  while (*p && line < lines) {
    int len = 1;
    if ((*p & 0xE0) == 0xC0) len = 2;
    else if ((*p & 0xF0) == 0xE0) len = 3;
    else if ((*p & 0xF8) == 0xF0) len = 4;
    char tmp[8] = {0};
    strncpy(tmp, (const char *)p, len);
    int w = M5.Lcd.textWidth(tmp);
    if (cx + w > x + maxW) {
      cx = x;
      if (++line >= lines) {
        M5.Lcd.setCursor(x, y + (lines - 1) * lineH);
        M5.Lcd.print("…");
        return;
      }
    }
    M5.Lcd.setCursor(cx, y + line * lineH);
    M5.Lcd.print(tmp);
    cx += w;
    p += len;
  }
}

static void poll() {
  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT + "/status";
  if (!http.begin(client, url)) { stState = "conn"; return; }
  http.setTimeout(2500);
  int code = http.GET();
  if (code != 200) { http.end(); stState = "conn"; return; }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) { stState = "conn"; return; }
  stTitle = String((const char *)(doc["title"] | ""));
  stProject = String((const char *)(doc["project"] | ""));
  stDone = doc["todos"]["done"] | 0;
  stTotal = doc["todos"]["total"] | 0;
  stCurrent = String((const char *)(doc["todos"]["current"] | ""));
  stState = String((const char *)(doc["state"] | "none"));
  stAgo = doc["ago"] | 0;
  tokAll = strtoull(String(doc["all_total"] | "0").c_str(), nullptr, 10);
  tokCache = strtoull(String(doc["cache"] | "0").c_str(), nullptr, 10);
  tokDayOut = strtoull(String(doc["day_out"] | "0").c_str(), nullptr, 10);
  planWin = doc["plan_win"] | 0;
  planQuota = doc["plan_quota"] | 600;
  tokMonth = strtoull(String(doc["month_total"] | "0").c_str(), nullptr, 10);
}

static void draw() {
  M5.Lcd.fillScreen(TFT_BLACK);

  // ---- 状态灯与标题栏 ----
  uint16_t stateCol = TFT_DARKGREY;
  const char *stateTxt = "";
  bool needBlink = false;
  if (stState == "running") { stateCol = TFT_GREEN; stateTxt = "运行中"; }
  else if (stState == "idle") { stateCol = TFT_YELLOW; stateTxt = "等你接入"; needBlink = true; }
  else if (stState == "error") { stateCol = TFT_RED; stateTxt = "出错"; }
  else if (stState == "conn") { stateCol = TFT_ORANGE; stateTxt = "守护进程失联"; }
  else if (stState == "none") { stateCol = TFT_DARKGREY; stateTxt = "无会话"; }
  bool lit = needBlink ? blinkOn : true;

  M5.Lcd.fillRect(0, 0, 240, 16, TFT_NAVY);
  M5.Lcd.setTextFont(1);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Lcd.setCursor(4, 5);
  M5.Lcd.print("ZCode");
  if (lit) M5.Lcd.fillCircle(40, 8, 4, stateCol);
  M5.Lcd.setTextColor(lit ? stateCol : TFT_DARKGREY, TFT_NAVY);
  M5.Lcd.setCursor(50, 5);
  M5.Lcd.print(stateTxt);
  int bat = M5.Power.getBatteryLevel();
  if (bat > 0) {
    M5.Lcd.setTextColor(bat <= 20 ? TFT_RED : TFT_LIGHTGREY, TFT_NAVY);
    M5.Lcd.setCursor(214, 5);
    M5.Lcd.printf("%d%%", bat);
  }

  // ---- 任务标题（两行） ----
  drawWrapped(stTitle.c_str(), 6, 22, 228, 2,
              stTitle.isEmpty() ? TFT_DARKGREY : TFT_CYAN, 16,
              &fonts::efontCN_12);

  // ---- Todo 进度 ----
  M5.Lcd.fillRect(6, 58, 180, 12, TFT_DARKGREY);
  if (stTotal > 0) {
    int w = 180 * stDone / stTotal;
    M5.Lcd.fillRect(6, 58, w, 12, stDone == stTotal ? TFT_GREEN : TFT_CYAN);
  }
  M5.Lcd.setTextFont(2);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.setCursor(192, 56);
  M5.Lcd.printf("%d/%d", stDone, stTotal);
  // 当前条目
  drawWrapped(stCurrent.isEmpty() ? "（无待办）" : stCurrent.c_str(),
              6, 76, 228, 1, TFT_LIGHTGREY, 15, &fonts::efontCN_12);

  // ---- Token 消耗 + plan 使用程度 ----
  M5.Lcd.setTextFont(1);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(6, 96);
  M5.Lcd.printf("month %s  cache %s  out %s",
                fmtTok(tokMonth).c_str(), fmtTok(tokCache).c_str(),
                fmtTok(tokDayOut).c_str());
  // plan 5h 窗口
  int pct = planQuota > 0 ? planWin * 100 / planQuota : 0;
  uint16_t pc = pct >= 90 ? TFT_RED : pct >= 70 ? TFT_ORANGE : TFT_GREEN;
  M5.Lcd.setCursor(6, 108);
  M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Lcd.printf("PLAN5h %d/%d", planWin, planQuota);
  M5.Lcd.fillRect(92, 109, 100, 7, TFT_DARKGREY);
  M5.Lcd.fillRect(92, 109, min(100, pct), 7, pc);
  M5.Lcd.setCursor(196, 108);
  M5.Lcd.setTextColor(pc, TFT_BLACK);
  M5.Lcd.printf("%d%%", pct);

  // ---- 底部提示 ----
  M5.Lcd.setCursor(6, 126);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  if (stState == "idle" && lit)
    M5.Lcd.print(">>> 需要你接入！去 Mac 看看 <<<");
  else
    M5.Lcd.print("A:亮度  B:刷新  B长按:休眠");
}

static void goSleep() {
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.sleep();
  M5.Power.M5pm1.setGPIOOutput(m5::M5PM1_Class::gpio2, false);
  delay(50);
  esp_sleep_enable_ext1_wakeup((1ULL << 11) | (1ULL << 12),
                               ESP_EXT1_WAKEUP_ANY_LOW);
  M5.Power.deepSleep(m5::Power_Class::sleep_no_timer);
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Lcd.setRotation(3);
  M5.Lcd.setBrightness(BRIGHTS[0]);
  Serial.begin(115200);
  Serial.printf("\r\n=== ZCode Dashboard ===\r\n");

  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Lcd.setCursor(40, 50);
  M5.Lcd.print("连接 WiFi ...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) M5.delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    M5.Lcd.setCursor(40, 74);
    M5.Lcd.setTextColor(TFT_RED, TFT_BLACK);
    M5.Lcd.print("WiFi 连接失败");
    delay(2000);
  }
}

void loop() {
  M5.update();
  static uint32_t lastPoll = 0, lastBlink = 0;

  if (M5.BtnA.wasClicked()) {
    brightIdx = (brightIdx + 1) % 3;
    M5.Lcd.setBrightness(BRIGHTS[brightIdx]);
  }
  if (M5.BtnB.wasClicked()) lastPoll = 0; // 立即刷新
  if (M5.BtnB.pressedFor(1200)) { goSleep(); return; }

  uint32_t now = millis();
  if (now - lastPoll > POLL_MS) {
    lastPoll = now;
    poll();
    draw();
  }
  if (now - lastBlink > 500) { // 等待接入的呼吸闪烁
    lastBlink = now;
    bool old = blinkOn;
    blinkOn = !blinkOn;
    if (stState == "idle" && old != blinkOn) draw();
  }
  delay(20);
}
