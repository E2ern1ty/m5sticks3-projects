/*
 * Punch Counter — M5StickS3 拳击出拳记录器
 *
 * 单击 A/B 键唤醒"开机"，立刻进入记录模式；
 * 挥拳（手持设备出拳或绑在手腕/拳套上）通过加速度峰值检测：
 *   每一次出拳计数 + 峰值 G 值（蜂鸣音调随力量升高）
 * 长按 A 键 1.2 秒"关机"（深度睡眠）；B 键单击清零本次训练；
 * 3 分钟无出拳无按键自动关机。
 *
 * 检测原理：|a| 超过 2.5g 视为一次出拳开始，持续追踪峰值，
 * 回落到 1.8g 以下并稳定 120ms 记为结束（防一次挥拳计多次）。
 */

#include <M5Unified.h>
#include <driver/rtc_io.h>

// ---------- 检测参数 ----------
static constexpr float TH_ON_G = 2.5f;    // 出拳触发阈值
static constexpr float TH_OFF_G = 1.8f;  // 结束阈值
static constexpr uint32_t STABLE_MS = 120;
static constexpr uint32_t MIN_EVENT_MS = 40;
static constexpr float G_BAR_MAX = 10.0f;
static constexpr uint32_t AUTO_SLEEP_MS = 3UL * 60 * 1000;

// ---------- 状态 ----------
static int punchCount = 0;
static float lastG = 0, maxG = 0;
static float hist[24] = {0};
static int histLen = 0, histHead = 0;
static uint32_t punchTimes[32]; // 最近出拳时刻（算频率）
static int ptLen = 0, ptHead = 0;

static bool inPunch = false;
static float eventPeak = 0;
static uint32_t eventStartMs = 0, belowSinceMs = 0;

static uint32_t sessionStartMs = 0;
static uint32_t lastActivityMs = 0;
static uint32_t lastUiMs = 0;
static bool flashPunch = false;

static void pushHist(float g) {
  hist[histHead % 24] = g;
  histHead++;
  if (histLen < 24) histLen++;
}
static void pushTime(uint32_t t) {
  punchTimes[ptHead % 32] = t;
  ptHead++;
  if (ptLen < 32) ptLen++;
}
static float punchRatePerMin() {
  uint32_t now = millis();
  int n = 0;
  for (int i = 0; i < ptLen; i++)
    if (now - punchTimes[(ptHead - ptLen + i) % 32] < 30000) n++;
  return n * 2.0f;
}

static uint16_t gColor(float g) {
  if (g >= 7) return TFT_RED;
  if (g >= 4.5) return TFT_ORANGE;
  if (g >= 3) return TFT_YELLOW;
  return TFT_GREEN;
}

// ---------- UI ----------
static void drawStatic() {
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(8, 4);
  M5.Lcd.print("PUNCH 出拳记录");
  M5.Lcd.setCursor(8, 118);
  M5.Lcd.print("B:清零 A长按:关机");
}

static void drawLive() {
  // 左：计数
  M5.Lcd.setFont(&fonts::Font4);
  M5.Lcd.fillRect(6, 22, 80, 30, TFT_BLACK);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.setCursor(6, 22);
  M5.Lcd.printf("%d", punchCount);
  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(6, 56);
  M5.Lcd.print("次数");
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f/分", punchRatePerMin());
  M5.Lcd.setCursor(6, 74);
  M5.Lcd.print(buf);

  // 中右：最近一击 G 值
  M5.Lcd.setFont(&fonts::Font4);
  M5.Lcd.fillRect(100, 22, 100, 30, TFT_BLACK);
  M5.Lcd.setTextColor(gColor(lastG), TFT_BLACK);
  M5.Lcd.setCursor(100, 22);
  M5.Lcd.printf("%.1fG", lastG);
  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(100, 56);
  snprintf(buf, sizeof(buf), "峰值 %.1fG", maxG);
  M5.Lcd.print(buf);
  uint32_t s = (millis() - sessionStartMs) / 1000;
  snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  M5.Lcd.setCursor(100, 74);
  M5.Lcd.print(buf);

  // G 值条
  M5.Lcd.fillRect(204, 22, 32, 66, TFT_DARKGREY);
  int h = (int)(fminf(fmaxf(lastG, 0), G_BAR_MAX) / G_BAR_MAX * 62);
  M5.Lcd.fillRect(204, 88 - h, 32, h, gColor(lastG));

  // 出拳历史柱状（底部）
  for (int i = 0; i < histLen; i++) {
    int idx = (histHead - histLen + i + 24 * 8) % 24;
    float g = hist[idx];
    int bh = (int)(fminf(g, G_BAR_MAX) / G_BAR_MAX * 26);
    int x = 6 + i * 10;
    M5.Lcd.fillRect(x, 106 - bh, 8, bh, gColor(g));
  }
  // 出拳瞬间的边框闪光
  if (flashPunch) {
    M5.Lcd.drawRect(0, 0, 240, 135, gColor(lastG));
    M5.Lcd.drawRect(1, 1, 238, 133, gColor(lastG));
    flashPunch = false;
  }
}

// ---------- 深度睡眠 ----------
static void goSleep() {
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setFont(&fonts::efontCN_16_b);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(90, 55);
  M5.Lcd.print("休眠");
  delay(400);
  M5.Lcd.sleep();
  esp_sleep_enable_ext1_wakeup((1ULL << 11) | (1ULL << 12), ESP_EXT1_WAKEUP_ANY_LOW);
  M5.Power.deepSleep(m5::Power_Class::sleep_no_timer);
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_imu = true;
  cfg.internal_spk = true;
  M5.begin(cfg);
  M5.Lcd.setRotation(3);
  M5.Lcd.setBrightness(90);
  M5.Speaker.setVolume(220);
  Serial.begin(115200);
  Serial.printf("\r\n=== Punch Counter ===\r\n");
  sessionStartMs = millis();
  lastActivityMs = millis();
  M5.Imu.begin();
  drawStatic();
  drawLive();
}

void loop() {
  M5.update();

  // B 单击：清零
  if (M5.BtnB.wasClicked()) {
    punchCount = 0; lastG = maxG = 0; histLen = histHead = 0; ptLen = ptHead = 0;
    sessionStartMs = millis();
    M5.Speaker.tone(1400, 60);
    M5.Lcd.fillRect(6, 82, 190, 24, TFT_BLACK);
    for (int i = 0; i < 24; i++) M5.Lcd.fillRect(6 + i * 10, 80, 8, 26, TFT_BLACK);
    lastActivityMs = millis();
  }
  // A 长按：关机
  if (M5.BtnA.pressedFor(1200)) { goSleep(); return; }

  // 采样加速度
  float ax, ay, az;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) { delay(2); return; }
  float m = sqrtf(ax * ax + ay * ay + az * az);
  uint32_t now = millis();

  if (!inPunch) {
    if (m > TH_ON_G) {
      inPunch = true;
      eventPeak = m;
      eventStartMs = now;
      belowSinceMs = 0;
    }
  } else {
    if (m > eventPeak) eventPeak = m;
    if (m < TH_OFF_G) {
      if (belowSinceMs == 0) belowSinceMs = now;
      if (now - belowSinceMs >= STABLE_MS) {
        // 出拳结束：登记
        if (now - eventStartMs >= MIN_EVENT_MS) {
          punchCount++;
          lastG = eventPeak;
          if (eventPeak > maxG) maxG = eventPeak;
          pushHist(eventPeak);
          pushTime(now);
          flashPunch = true;
          int f = 1200 + (int)(fminf(eventPeak, 10) * 180);
          M5.Speaker.tone(f, 45);
          Serial.printf("[punch] #%d %.2fG\r\n", punchCount, eventPeak);
          lastActivityMs = now;
        }
        inPunch = false;
      }
    } else {
      belowSinceMs = 0;
    }
  }

  if (now - lastUiMs > 150) {
    lastUiMs = now;
    drawLive();
  }
  // 自动关机
  if (now - lastActivityMs > AUTO_SLEEP_MS) { goSleep(); return; }

  delay(2); // ~500Hz 采样
}
