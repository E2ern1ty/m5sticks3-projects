/*
 * Punch Coach — M5StickS3 拳法教练（v2）
 *
 * 两种模式（B 键切换）：
 *   FREE  自由练习：出拳计数 + 峰值 G + 频率 + 历史柱状（原版功能，检测提速）
 *   COMBO 拳法训练：内置经典组合拳，喇叭离线语音（ESP8266SAM，无需联网）
 *         依次喊出每一拳的顺序号，检测你的出拳并计分
 *
 * 拳法编号（拳击通用）：1=刺拳 2=后手直拳 3=前手勾拳 4=后手勾拳 5=前手上勾 6=后手上勾
 * 内置组合：1-2 / 1-1-2 / 1-2-3 / 1-2-3-2 / 1-2-5-6
 *
 * 按键：  单击 A/B 唤醒"开机"；长按 A 关机
 *         B 单击 切换模式；COMBO 里 A 单击 换下一个组合；B 长按 清计分
 *         3 分钟无出拳自动关机
 *
 * 快速出拳适配：事件窗口收紧（回落 1.8g 稳定 70ms 即记一次，最短 25ms），
 * ~800Hz 采样，连击间隔 >120ms 都能分开。
 */

#include <M5Unified.h>
#include <ESP8266SAM.h>
#include <AudioOutput.h>

// ---------- 检测参数 ----------
static constexpr float TH_ON_G = 2.5f;
static constexpr float TH_OFF_G = 1.8f;
static constexpr uint32_t STABLE_MS = 70;    // 收紧：快速连击也能分开
static constexpr uint32_t MIN_EVENT_MS = 25;
static constexpr float G_BAR_MAX = 10.0f;
static constexpr uint32_t AUTO_SLEEP_MS = 3UL * 60 * 1000;
static constexpr uint32_t STEP_TIMEOUT_MS = 4000;

// ---------- 拳法与组合 ----------
static const char *PUNCH_NAME[7] = {"", "Jab", "Cross", "Hook", "Hook",
                                    "Upper", "Upper"};
static const char *PUNCH_CN[7] = {"", "刺拳", "直拳", "勾拳", "后勾",
                                  "上勾", "后上勾"};
static const char *PUNCH_SAY[7] = {"", "One", "Two", "Three", "Four",
                                   "Five", "Six"};
struct Combo {
  const char *name;
  uint8_t steps[6];
  int n;
};
static const Combo COMBOS[] = {
  {"1-2",     {1, 2}, 2},
  {"1-1-2",   {1, 1, 2}, 3},
  {"1-2-3",   {1, 2, 3}, 3},
  {"1-2-3-2", {1, 2, 3, 2}, 4},
  {"1-2-5-6", {1, 2, 5, 6}, 4},
};
static const int N_COMBOS = sizeof(COMBOS) / sizeof(COMBOS[0]);

// ---------- 状态 ----------
enum Mode { MODE_FREE, MODE_COMBO };
static Mode mode = MODE_FREE;
static int comboIdx = 0, comboStep = 0;
static int hits = 0, total = 0;          // COMBO 计分
static bool waitingPunch = false;
static uint32_t stepDeadline = 0;

static int punchCount = 0;
static float lastG = 0, maxG = 0;
static float hist[24] = {0};
static int histLen = 0, histHead = 0;
static uint32_t punchTimes[32];
static int ptLen = 0, ptHead = 0;

static bool inPunch = false;
static float eventPeak = 0;
static uint32_t eventStartMs = 0, belowSinceMs = 0;
static uint32_t sessionStartMs = 0, lastActivityMs = 0, lastUiMs = 0;
static bool flashPunch = false;

// ---------- 语音（SAM -> PSRAM -> 喇叭） ----------
static ESP8266SAM sam;
class SpkSink : public AudioOutput {
public:
  int16_t *buf = nullptr;
  size_t cap = 0, len = 0;
  uint32_t rate = 22050;
  bool begin() override { len = 0; return true; }
  bool stop() override { return true; }
  bool SetRate(int hz) override { rate = hz; return true; }
  bool ConsumeSample(int16_t s[2]) override {
    if (len < cap) { buf[len++] = s[0]; return true; }
    return false;
  }
};
static SpkSink sink;

static void say(const char *text) {
  if (!sink.buf) {
    sink.cap = 22050 * 4; // ~4 秒
    sink.buf = (int16_t *)ps_malloc(sink.cap * sizeof(int16_t));
    if (!sink.buf) return;
  }
  Serial.printf("[say] %s\r\n", text);
  sam.Say(&sink, text);
  if (sink.len == 0) return;
  M5.Speaker.playRaw(sink.buf, sink.len, sink.rate, false);
  uint32_t t0 = millis();
  while (M5.Speaker.isPlaying() && millis() - t0 < 6000) {
    M5.update();
    delay(10);
  }
}

// ---------- 小工具 ----------
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
  if (g >= 4.5f) return TFT_ORANGE;
  if (g >= 3) return TFT_YELLOW;
  return TFT_GREEN;
}

// ---------- UI ----------
static void drawFreeStatic() {
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(8, 4);
  M5.Lcd.print("FREE 自由出拳");
  M5.Lcd.setCursor(8, 118);
  M5.Lcd.print("B:模式 A长按:关机");
}
static void drawComboStatic() {
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Lcd.setCursor(8, 4);
  M5.Lcd.print("COMBO ");
  M5.Lcd.print(COMBOS[comboIdx].name);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(8, 118);
  M5.Lcd.print("A:换组合 B:自由");
}
static void drawFreeLive() {
  char buf[20];
  M5.Lcd.setFont(&fonts::Font4);
  M5.Lcd.fillRect(6, 22, 80, 30, TFT_BLACK);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.setCursor(6, 22);
  M5.Lcd.printf("%d", punchCount);
  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(6, 56);
  M5.Lcd.print("次数");
  snprintf(buf, sizeof(buf), "%.0f/分", punchRatePerMin());
  M5.Lcd.setCursor(6, 74);
  M5.Lcd.print(buf);
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
  M5.Lcd.fillRect(204, 22, 32, 66, TFT_DARKGREY);
  int h = (int)(fminf(fmaxf(lastG, 0), G_BAR_MAX) / G_BAR_MAX * 62);
  M5.Lcd.fillRect(204, 88 - h, 32, h, gColor(lastG));
  for (int i = 0; i < histLen; i++) {
    int idx = (histHead - histLen + i + 24 * 8) % 24;
    int bh = (int)(fminf(hist[idx], G_BAR_MAX) / G_BAR_MAX * 26);
    M5.Lcd.fillRect(6 + i * 10, 106 - bh, 8, bh, gColor(hist[idx]));
  }
  if (flashPunch) {
    M5.Lcd.drawRect(0, 0, 240, 135, gColor(lastG));
    M5.Lcd.drawRect(1, 1, 238, 133, gColor(lastG));
    flashPunch = false;
  }
}
static void drawComboLive() {
  const Combo &c = COMBOS[comboIdx];
  char buf[24];
  // 当前期待的拳：大号数字 + 名称
  M5.Lcd.setFont(&fonts::Font7); // 七段大数字
  M5.Lcd.fillRect(20, 22, 60, 60, TFT_BLACK);
  M5.Lcd.setTextColor(waitingPunch ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(28, 22);
  if (comboStep < c.n) M5.Lcd.printf("%d", c.steps[comboStep]);
  M5.Lcd.setFont(&fonts::efontCN_16_b);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.setCursor(95, 34);
  M5.Lcd.fillRect(95, 30, 100, 24, TFT_BLACK);
  if (comboStep < c.n) M5.Lcd.print(PUNCH_CN[c.steps[comboStep]]);
  // 进度点
  for (int i = 0; i < c.n; i++) {
    uint16_t col = i < comboStep ? TFT_GREEN
                  : (i == comboStep ? TFT_YELLOW : TFT_DARKGREY);
    M5.Lcd.fillCircle(100 + i * 18, 70, 5, col);
  }
  // 计分 + 力度
  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(95, 84);
  snprintf(buf, sizeof(buf), "命中 %d/%d", hits, total);
  M5.Lcd.print(buf);
  M5.Lcd.setCursor(6, 88);
  snprintf(buf, sizeof(buf), "%.1fG", lastG);
  M5.Lcd.setTextColor(gColor(lastG), TFT_BLACK);
  M5.Lcd.fillRect(6, 84, 60, 18, TFT_BLACK);
  M5.Lcd.setCursor(6, 84);
  M5.Lcd.print(buf);
  if (flashPunch) {
    M5.Lcd.drawRect(0, 0, 240, 135, gColor(lastG));
    flashPunch = false;
  }
}

// ---------- COMBO 流程 ----------
static void comboStart() {
  comboStep = 0;
  waitingPunch = false;
  drawComboStatic();
  drawComboLive();
}
static void comboNextStep() {
  const Combo &c = COMBOS[comboIdx];
  if (comboStep >= c.n) return;
  waitingPunch = true;
  stepDeadline = millis() + STEP_TIMEOUT_MS;
  say(PUNCH_SAY[c.steps[comboStep]]);
}
static void comboOnPunch() {
  if (!waitingPunch) return;
  waitingPunch = false;
  hits++;
  total++;
  const Combo &c = COMBOS[comboIdx];
  comboStep++;
  M5.Speaker.tone(2800, 15);
  if (comboStep >= c.n) {
    say("Good job");
    delay(600);
    comboStart();          // 再来一轮同组合
    comboNextStep();
  } else {
    comboNextStep();       // 立刻喊下一拳
  }
}
static void comboTimeout() {
  waitingPunch = false;
  total++;
  say("Miss");
  comboNextStep();         // 超时也算一步，继续
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
  esp_sleep_enable_ext1_wakeup((1ULL << 11) | (1ULL << 12),
                               ESP_EXT1_WAKEUP_ANY_LOW);
  M5.Power.deepSleep(m5::Power_Class::sleep_no_timer);
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_imu = true;
  cfg.internal_spk = true;
  M5.begin(cfg);
  M5.Lcd.setRotation(3);
  M5.Lcd.setBrightness(90);
  M5.Speaker.setVolume(255);
  Serial.begin(115200);
  Serial.printf("\r\n=== Punch Coach v2 ===\r\n");
  sessionStartMs = lastActivityMs = millis();
  M5.Imu.begin();
  drawFreeStatic();
  drawFreeLive();
}

void loop() {
  M5.update();

  // A 长按关机
  if (M5.BtnA.pressedFor(1200)) { goSleep(); return; }

  // B 单击：模式切换
  if (M5.BtnB.wasClicked()) {
    mode = (mode == MODE_FREE) ? MODE_COMBO : MODE_FREE;
    M5.Speaker.tone(1800, 40);
    if (mode == MODE_COMBO) {
      hits = total = 0;
      comboStart();
      comboNextStep();
    } else {
      drawFreeStatic();
      drawFreeLive();
    }
    lastActivityMs = millis();
  }
  // COMBO 里 A 单击：换组合；B 长按：清计分
  if (mode == MODE_COMBO) {
    if (M5.BtnA.wasClicked()) {
      comboIdx = (comboIdx + 1) % N_COMBOS;
      hits = total = 0;
      M5.Speaker.tone(2000, 40);
      comboStart();
      comboNextStep();
      lastActivityMs = millis();
    }
    if (M5.BtnB.pressedFor(800)) {
      hits = total = 0;
      drawComboStatic();
      drawComboLive();
      while (M5.BtnB.isPressed()) { M5.update(); delay(10); }
    }
  } else {
    // FREE 里 B 长按清零（原行为）
    if (M5.BtnB.pressedFor(800)) {
      punchCount = 0; lastG = maxG = 0; histLen = histHead = 0; ptLen = ptHead = 0;
      sessionStartMs = millis();
      drawFreeStatic();
      drawFreeLive();
      while (M5.BtnB.isPressed()) { M5.update(); delay(10); }
    }
  }

  // COMBO 步骤超时
  if (mode == MODE_COMBO && waitingPunch && millis() > stepDeadline) {
    comboTimeout();
    lastActivityMs = millis();
  }

  // 采样加速度（~800Hz）
  float ax, ay, az;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) { delay(1); return; }
  float m = sqrtf(ax * ax + ay * ay + az * az);
  uint32_t now = millis();
  bool newPunch = false;

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
        if (now - eventStartMs >= MIN_EVENT_MS) newPunch = true;
        inPunch = false;
      }
    } else {
      belowSinceMs = 0;
    }
  }

  if (newPunch) {
    punchCount++;
    lastG = eventPeak;
    if (eventPeak > maxG) maxG = eventPeak;
    pushHist(eventPeak);
    pushTime(now);
    flashPunch = true;
    if (mode == MODE_FREE) {
      int f = 1200 + (int)(fminf(eventPeak, 10) * 180);
      M5.Speaker.tone(f, 45);
    }
    Serial.printf("[punch] #%d %.2fG mode=%d\r\n", punchCount, eventPeak, (int)mode);
    lastActivityMs = now;
    if (mode == MODE_COMBO) comboOnPunch();
  }

  if (now - lastUiMs > 150) {
    lastUiMs = now;
    if (mode == MODE_FREE) drawFreeLive();
    else drawComboLive();
  }
  if (now - lastActivityMs > AUTO_SLEEP_MS) { goSleep(); return; }

  delay(1);
}
