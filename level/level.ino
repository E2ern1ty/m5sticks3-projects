/*
 * Bubble Level — M5StickS3 水平仪
 *
 * 把棒子平放在桌面上检查桌子水平：
 *   屏幕左侧是气泡水平仪：球会滚向低的一侧（垫高球所在的那边），桌平则球居中
 *   屏幕右侧是数字角度 X/Y（相对水平面的倾角，度）
 *
 * 按键：  A 短按  切换量程 ±2° / ±5° / ±10°（检查桌面建议 ±2°）
 *         B 短按  以当前姿态为零点校准（放到已知水平面上按一次）
 *         B 长按  清除校准
 * 水平判定：两轴倾角均 < 0.3° 时球变绿并蜂鸣一次（进入时触发）
 */

#include <M5Unified.h>
#include <Preferences.h>

static const int SCREEN_W = 240, SCREEN_H = 135;
static const float LEVEL_ZONE = 0.3f;   // 度，水平判定阈值
static const float RANGES[] = {2.0f, 5.0f, 10.0f};
static const int N_RANGES = 3;
static int rangeIdx = 0;

static float ax0 = 0, ay0 = 0;          // 校准零点（g）
static bool hasCalib = false;

// 平滑后的加速度（g）
static float sx = 0, sy = 0, sz = 1;
static bool inLevelZone = false;

static const int CX = 68, CY = 66, R = 56; // 气泡仪圆心与半径

static void drawStatic() {
  M5.Lcd.fillScreen(TFT_BLACK);
  // 同心圆 + 十字
  M5.Lcd.drawCircle(CX, CY, R, TFT_DARKGREY);
  M5.Lcd.drawCircle(CX, CY, R * 2 / 3, TFT_DARKGREY);
  M5.Lcd.drawCircle(CX, CY, R / 3, TFT_DARKGREY);
  M5.Lcd.drawFastVLine(CX, CY - R, R * 2, TFT_DARKGREY);
  M5.Lcd.drawFastHLine(CX - R, CY, R * 2, TFT_DARKGREY);
  // 水平判定圈（±0.3° 映射的绿色圈）
  float zr = LEVEL_ZONE / RANGES[rangeIdx] * R;
  M5.Lcd.drawCircle(CX, CY, (int)(zr < 4 ? 4 : zr), TFT_DARKGREEN);
  // 右侧信息区
  M5.Lcd.setTextFont(1);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(140, 8);
  M5.Lcd.print("TILT / deg");
  M5.Lcd.setCursor(140, 100);
  M5.Lcd.printf("range +-%.0f", RANGES[rangeIdx]);
  M5.Lcd.setCursor(140, 112);
  M5.Lcd.print("A:range  B:zero");
}

static void draw() {
  float ax = sx - (hasCalib ? ax0 : 0);
  float ay = sy - (hasCalib ? ay0 : 0);
  float az = sz;
  // 倾角（度）
  float dx = atan2f(ax, az) * 57.29578f;
  float dy = atan2f(ay, az) * 57.29578f;
  bool lv = fabsf(dx) < LEVEL_ZONE && fabsf(dy) < LEVEL_ZONE;
  if (lv && !inLevelZone) M5.Speaker.tone(2600, 90); // 进入水平区提示
  inLevelZone = lv;

  // 球位置：滚向低处（角度正方向取负让"球往低侧滚"的直觉成立）
  float px = -dx / RANGES[rangeIdx] * R;
  float py = -dy / RANGES[rangeIdx] * R;
  float m = sqrtf(px * px + py * py);
  if (m > R - 6) { px *= (R - 6) / m; py *= (R - 6) / m; } // 出界贴边
  int bx = CX + (int)px, by = CY + (int)py;

  // 清旧球画新球（只擦小球区域，避免整屏闪）
  static int ox = -99, oy = -99;
  if (ox >= 0) {
    M5.Lcd.fillCircle(ox, oy, 6, TFT_BLACK);
    // 补回被球盖住的参考线段
    if (abs(oy - CY) < 6) M5.Lcd.drawFastHLine(CX - R, CY, R * 2, TFT_DARKGREY);
    if (abs(ox - CX) < 6) M5.Lcd.drawFastVLine(CX, CY - R, R * 2, TFT_DARKGREY);
    if (abs(ox - CX) < 6 && abs(oy - CY) < 6) M5.Lcd.drawCircle(CX, CY, 2, TFT_DARKGREEN);
  }
  M5.Lcd.fillCircle(bx, by, 6, lv ? TFT_GREEN : TFT_CYAN);
  M5.Lcd.drawCircle(bx, by, 6, TFT_WHITE);
  ox = bx; oy = by;

  // 数字区
  M5.Lcd.setTextFont(4);
  M5.Lcd.setTextColor(lv ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
  M5.Lcd.fillRect(140, 22, 96, 28, TFT_BLACK);
  M5.Lcd.setCursor(140, 22);
  M5.Lcd.printf("%+05.1fX", dx);
  M5.Lcd.fillRect(140, 54, 96, 28, TFT_BLACK);
  M5.Lcd.setCursor(140, 54);
  M5.Lcd.printf("%+05.1fY", dy);

  M5.Lcd.setTextFont(2);
  M5.Lcd.setTextColor(lv ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.fillRect(140, 84, 96, 14, TFT_BLACK);
  M5.Lcd.setCursor(140, 84);
  M5.Lcd.print(lv ? "LEVEL!" : (hasCalib ? "calibrated" : "raw"));
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_imu = true;
  cfg.internal_spk = true;
  M5.begin(cfg);
  M5.Lcd.setRotation(3);
  M5.Lcd.setBrightness(100);
  M5.Speaker.setVolume(200);

  Serial.begin(115200);
  Serial.printf("\r\n=== Bubble Level for M5StickS3 ===\r\n");

  Preferences p;
  p.begin("level", true);
  hasCalib = p.getUChar("cal", 0) != 0;
  if (hasCalib) {
    ax0 = p.getFloat("ax0", 0);
    ay0 = p.getFloat("ay0", 0);
  }
  p.end();

  M5.Imu.begin(); // BMI270
  drawStatic();
  // 初始平滑值直接取一次采样
  M5.Imu.getAccel(&sx, &sy, &sz);
}

void loop() {
  M5.update();

  // 采样 + EMA 平滑（20ms 一次）
  float ax, ay, az;
  if (M5.Imu.getAccel(&ax, &ay, &az)) {
    sx += 0.15f * (ax - sx);
    sy += 0.15f * (ay - sy);
    sz += 0.15f * (az - sz);
  }

  // A 短按：切换量程
  if (M5.BtnA.wasClicked()) {
    rangeIdx = (rangeIdx + 1) % N_RANGES;
    drawStatic();
  }
  // B 短按：以当前姿态为零点
  if (M5.BtnB.wasClicked()) {
    ax0 = sx; ay0 = sy; hasCalib = true;
    Preferences p;
    p.begin("level", false);
    p.putUChar("cal", 1);
    p.putFloat("ax0", ax0);
    p.putFloat("ay0", ay0);
    p.end();
    M5.Speaker.tone(2000, 60);
    Serial.printf("[cal] zero @ %.4f %.4f\r\n", ax0, ay0);
  }
  // B 长按：清除校准
  if (M5.BtnB.pressedFor(1000)) {
    hasCalib = false;
    Preferences p;
    p.begin("level", false);
    p.putUChar("cal", 0);
    p.end();
    M5.Speaker.tone(1200, 60);
    Serial.println("[cal] cleared");
    drawStatic();
  }

  draw();
  delay(20);
}
