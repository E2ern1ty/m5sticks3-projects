/*
 * Pet Talk — M5StickS3 猫狗鸟沟通器
 *
 * 内置 9 种合成叫声（三物种 × 三种"语言"），选择物种与叫声后播放。
 * 纯离线，无麦克风无联网。
 *
 * 猫：喵 ~ / 呼噜~ / 哈气！
 * 狗：汪汪！ / 低吼… / 呜咽~
 * 鸟：啾啾！ / 口哨~ / 咕咕~
 *
 * 按键：  单击 A/B 唤醒；A=下一项；B=确认/进入下一级/播放
 *         长按 A 关机（深度睡眠）
 * 流程：  选物种 -> 选叫声 -> 播放(循环闪音符) -> B 返回叫声选择
 */

#include <M5Unified.h>
#include "sounds.h"

enum Species { SP_CAT = 0, SP_DOG, SP_BIRD };
static const char *SP_NAMES[] = {"猫咪", "狗狗", "鸟鸟"};
// 每物种 3 种叫声：名称 / 数据 / 长度
struct Sound {
  const char *name;
  const int16_t *data;
  size_t len;
};
static const Sound SOUNDS[3][3] = {
  { {"喵~",    SND_MEOW,    SND_MEOW_LEN},
    {"呼噜噜", SND_PURR,    SND_PURR_LEN},
    {"哈气!",  SND_HISS,    SND_HISS_LEN} },
  { {"汪汪!",  SND_BARK,    SND_BARK_LEN},
    {"低吼…",  SND_GROWL,   SND_GROWL_LEN},
    {"呜咽~",  SND_WHINE,   SND_WHINE_LEN} },
  { {"啾啾!",  SND_CHIRP,   SND_CHIRP_LEN},
    {"口哨~",  SND_WHISTLE, SND_WHISTLE_LEN},
    {"咕咕~",  SND_COO,     SND_COO_LEN} },
};

static int species = 0, soundIdx = 0;
enum Scr { SCR_SPECIES, SCR_SOUND, SCR_PLAY };
static Scr scr = SCR_SPECIES;

// ---------- 像素脸 ----------
static void drawFace(int cx, int cy, int sp, uint16_t col) {
  switch (sp) {
  case SP_CAT: // 圆脸+三角耳
    M5.Lcd.fillTriangle(cx - 28, cy - 18, cx - 10, cy - 22, cx - 24, cy - 38, col);
    M5.Lcd.fillTriangle(cx + 28, cy - 18, cx + 10, cy - 22, cx + 24, cy - 38, col);
    M5.Lcd.fillCircle(cx, cy, 26, col);
    M5.Lcd.fillCircle(cx - 9, cy - 4, 3, TFT_BLACK);
    M5.Lcd.fillCircle(cx + 9, cy - 4, 3, TFT_BLACK);
    M5.Lcd.fillTriangle(cx - 3, cy + 3, cx + 3, cy + 3, cx, cy + 8, TFT_BLACK);
    break;
  case SP_DOG: // 圆脸+垂耳
    M5.Lcd.fillEllipse(cx - 26, cy - 8, 9, 18, col);
    M5.Lcd.fillEllipse(cx + 26, cy - 8, 9, 18, col);
    M5.Lcd.fillCircle(cx, cy, 24, col);
    M5.Lcd.fillCircle(cx - 8, cy - 4, 3, TFT_BLACK);
    M5.Lcd.fillCircle(cx + 8, cy - 4, 3, TFT_BLACK);
    M5.Lcd.fillEllipse(cx, cy + 9, 6, 4, TFT_BLACK);
    break;
  case SP_BIRD: // 圆脸+尖喙+头冠
    M5.Lcd.fillCircle(cx, cy, 22, col);
    M5.Lcd.fillTriangle(cx - 4, cy - 22, cx + 4, cy - 22, cx, cy - 36, TFT_ORANGE);
    M5.Lcd.fillTriangle(cx, cy, cx + 14, cy - 5, cx, cy + 7, TFT_ORANGE);
    M5.Lcd.fillCircle(cx - 8, cy - 5, 3, TFT_BLACK);
    break;
  }
}

static void drawSpeciesScr() {
  M5.Lcd.fillScreen(TFT_BLACK);
  // 三个物种横排，当前高亮放大
  for (int i = 0; i < 3; i++) {
    bool cur = (i == species);
    int cx = 40 + i * 80, cy = 56;
    drawFace(cx, cy, i, cur ? TFT_WHITE : TFT_DARKGREY);
    M5.Lcd.setFont(&fonts::efontCN_16_b);
    M5.Lcd.setTextColor(cur ? TFT_CYAN : TFT_DARKGREY, TFT_BLACK);
    M5.Lcd.setCursor(cx - 16, 96);
    M5.Lcd.print(SP_NAMES[i]);
    if (cur) M5.Lcd.drawRect(cx - 34, cy - 44, 68, 78, TFT_CYAN);
  }
  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(16, 118);
  M5.Lcd.print("A:换物种 B:选择");
}

static void drawSoundScr() {
  M5.Lcd.fillScreen(TFT_BLACK);
  drawFace(34, 46, species, TFT_WHITE);
  M5.Lcd.setFont(&fonts::efontCN_16_b);
  M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Lcd.setCursor(76, 8);
  M5.Lcd.print(SP_NAMES[species]);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(76, 28);
  M5.Lcd.print("选叫声:");
  for (int i = 0; i < 3; i++) {
    bool cur = (i == soundIdx);
    M5.Lcd.setFont(&fonts::efontCN_16_b);
    M5.Lcd.setTextColor(cur ? TFT_WHITE : TFT_DARKGREY, TFT_BLACK);
    M5.Lcd.setCursor(80, 48 + i * 24);
    M5.Lcd.printf("%d %s", i + 1, SOUNDS[species][i].name);
    if (cur) M5.Lcd.fillRect(72, 46 + i * 24, 100, 22, 0x2104);
    if (cur) M5.Lcd.drawRect(72, 46 + i * 24, 100, 22, TFT_CYAN);
  }
  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(16, 118);
  M5.Lcd.print("A:下一个 B:播放");
}

static void drawPlayScr(bool phase) {
  M5.Lcd.fillScreen(TFT_BLACK);
  drawFace(120, 50, species, TFT_WHITE);
  // 音符动画
  M5.Lcd.setFont(&fonts::efontCN_16_b);
  M5.Lcd.setTextColor(TFT_ORANGE, TFT_BLACK);
  for (int i = 0; i < 3; i++) {
    int x = 70 + i * 46;
    int y = phase ? 76 : 84;
    M5.Lcd.fillCircle(x, y + 6, 4, TFT_ORANGE);
    M5.Lcd.drawFastVLine(x + 4, y - 6, 12, TFT_ORANGE);
  }
  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Lcd.setCursor(96, 106);
  M5.Lcd.printf("%s %s", SP_NAMES[species], SOUNDS[species][soundIdx].name);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(16, 122);
  M5.Lcd.print("B:返回 A:重播");
}

static void playSound() {
  const Sound &s = SOUNDS[species][soundIdx];
  M5.Speaker.playRaw(s.data, s.len, 16000, false);
  Serial.printf("[play] %s %s\r\n", SP_NAMES[species], s.name);
}

static void goSleep() {
  M5.Lcd.fillScreen(TFT_BLACK);
  delay(200);
  M5.Lcd.sleep();
  esp_sleep_enable_ext1_wakeup((1ULL << 11) | (1ULL << 12),
                               ESP_EXT1_WAKEUP_ANY_LOW);
  M5.Power.deepSleep(m5::Power_Class::sleep_no_timer);
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = true;
  M5.begin(cfg);
  M5.Lcd.setRotation(3);
  M5.Lcd.setBrightness(100);
  M5.Speaker.setVolume(255);
  Serial.begin(115200);
  Serial.printf("\r\n=== Pet Talk ===\r\n");
  drawSpeciesScr();
}

void loop() {
  M5.update();

  if (M5.BtnA.pressedFor(1200)) { goSleep(); return; }

  if (M5.BtnA.wasClicked()) {
    M5.Speaker.tone(1800, 30);
    if (scr == SCR_SPECIES) {
      species = (species + 1) % 3;
      soundIdx = 0;
      drawSpeciesScr();
    } else if (scr == SCR_SOUND) {
      soundIdx = (soundIdx + 1) % 3;
      drawSoundScr();
    } else if (scr == SCR_PLAY) {
      playSound(); // 重播
    }
  }
  if (M5.BtnB.wasClicked()) {
    M5.Speaker.tone(2400, 30);
    if (scr == SCR_SPECIES) {
      scr = SCR_SOUND;
      drawSoundScr();
    } else if (scr == SCR_SOUND) {
      scr = SCR_PLAY;
      drawPlayScr(false);
      playSound();
    } else {
      scr = SCR_SOUND;
      drawSoundScr();
    }
  }

  // 播放中的音符动画
  if (scr == SCR_PLAY) {
    static uint32_t lastAnim = 0;
    static bool ph = false;
    if (M5.Speaker.isPlaying()) {
      if (millis() - lastAnim > 250) {
        lastAnim = millis();
        ph = !ph;
        drawPlayScr(ph);
      }
    } else if (lastAnim != 0) {
      lastAnim = 0;
      drawPlayScr(false);
    }
  }

  delay(10);
}
