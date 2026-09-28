/*
 * WiFi Signal Hunter — M5StickS3 (v4)
 *
 * 扫描周边 WiFi 信号源，按信号强度排序；选中任一信号源后进入
 * 追踪模式：实时 dBm、历史曲线、信号条，以及“盖革计数器”式的
 * 蜂鸣反馈——离信号源越近响得越急，凭声音就能摸到 AP 的位置。
 *
 * v4：列表顺序固定（首扫按信号排序后位置不变，新信号源追加到
 *     末尾，连续 3 轮扫不到才移除）；标题栏两行——上为 scanning
 *     状态、下为实时电量；列表滚动 + 只重绘变化的行（不整屏闪）。
 * v3：追踪页单信道快扫（~4Hz），显示值 EMA 平滑。
 *
 * 按键：  BtnA(正面大键 G11)  列表页=下移选择(可滚动) / 追踪页=全扫
 *         BtnB(侧面键  G12)  列表页=进入追踪 / 追踪页=返回列表
 * 重力：  BMI270 检测横屏方向，倒拿 180° 屏幕自动翻转。
 */

#include <M5Unified.h>
#include <WiFi.h>

// ---------- 界面常量 ----------
enum UiMode { MODE_LIST, MODE_TRACK };

static const int SCREEN_W = 240;
static const int SCREEN_H = 135;
static const int LIST_HEADER_H = 24;   // 两行：scanning + 电量
static const int LIST_ROW_H = 15;
static const int LIST_ROWS = (SCREEN_H - LIST_HEADER_H) / LIST_ROW_H; // 7 行

static const int MAX_APS = 32;
static const int AP_DROP_MISSES = 3;           // 连续多少轮扫不到才移除
static const int TREND_N = 110;
static const uint32_t LIST_RESCAN_DELAY_MS = 400;
static const uint32_t TRACK_RESCAN_DELAY_MS = 80;
static const uint32_t TRACK_FULL_SCAN_MS = 15000;
static const uint32_t FULL_DWELL_MS = 180;
static const uint32_t FAST_DWELL_MS = 120;
static const int TRACK_MISS_LIMIT = 8;

static constexpr float EMA_ALPHA = 0.4f;

// IMU 翻转：ax 为横屏下的“上下”轴。方向反了就改成 -1.0f。
static constexpr float AX_SIGN = 1.0f;
static constexpr float ROT_THRESHOLD_G = 0.35f;
static const uint32_t ROT_STABLE_MS = 500;

// ---------- 扫描结果 ----------
struct ApInfo {
  char ssid[33];
  uint8_t bssid[6];
  int16_t rssi;
  uint8_t channel;
  bool hidden;
  uint8_t miss;   // 连续未扫到的轮数
};

// ---------- 全局状态 ----------
static UiMode mode = MODE_LIST;
static ApInfo aps[MAX_APS];
static int apCount = 0;
static bool firstSortDone = false;
static bool showHidden = true;        // 列表是否显示隐藏 SSID 的信号源
static bool hiddenToggleLatch = false;
static int visIdx[MAX_APS];           // 可见槽位 -> aps[] 下标
static int visCount = 0;
static int cursor = 0;               // 选中条目（可见槽位空间 0..visCount-1）
static int topRow = 0;               // 滚动窗口首行
static uint8_t targetBssid[6] = {0};
static char targetName[33] = {0};
static bool targetHidden = false;
static uint8_t targetChannel = 0;
static int targetRssi = -100;
static float targetRssiEma = -100.0f;
static bool targetLost = false;
static int trackMiss = 0;
static unsigned long lastFullScanMs = 0;

static bool navOn = false; // 3D 导航罗盘开关（追踪页）
static int trend[TREND_N];
static int trendLen = 0;
static unsigned long trendHead = 0;
static unsigned long lastSampleMs = 0;
static bool scanning = false;
static bool scanningFast = false;
static unsigned long nextScanMs = 0;
static unsigned long nextBeepMs = 0;
static unsigned long nextRotCheckMs = 0;
static uint8_t rotCur = 3;
static uint8_t rotCandidate = 3;
static unsigned long rotCandidateSince = 0;

// 列表重绘记账：只重绘发生变化的行
static int16_t drawnRssi[LIST_ROWS];
static uint8_t drawnCh[LIST_ROWS];
static int drawnCursorRow = -1;
static int drawnRows = 0;
static int drawnTop = -1;
static bool drawnScanning = false;
static int drawnBat = -999;
static bool listDirty = true;

// ---------- 小工具 ----------
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static uint16_t rssiColor(int rssi) {
  if (rssi >= -55) return TFT_GREEN;
  if (rssi >= -70) return TFT_YELLOW;
  if (rssi >= -80) return TFT_ORANGE;
  return TFT_RED;
}

static const char *rssiWord(int rssi) {
  if (rssi >= -50) return "Amazing";
  if (rssi >= -65) return "Good";
  if (rssi >= -75) return "Fair";
  if (rssi >= -85) return "Weak";
  return "Barely";
}

static void pushTrend(int rssi) {
  trend[trendHead % TREND_N] = rssi;
  trendHead++;
  if (trendLen < TREND_N) trendLen++;
}

// ---------- 扫描管理 ----------
static void startScan(bool fast) {
  if (scanning) return;
  if (fast && targetChannel >= 1 && targetChannel <= 13) {
    WiFi.scanNetworks(true, true, false, FAST_DWELL_MS, targetChannel);
    scanningFast = true;
  } else {
    WiFi.scanNetworks(true, true, false, FULL_DWELL_MS);
    scanningFast = false;
    lastFullScanMs = millis();
  }
  scanning = true;
  updateListHeader();
}

// 把刚完成的全扫结果合并进 aps[]：位置固定，原地更新，新条目追加
static void mergeScanIntoRegistry() {
  int n = WiFi.scanComplete();
  if (n <= 0) return;
  const bool firstScan = !firstSortDone;

  for (int i = 0; i < apCount; i++) aps[i].miss++;

  for (int i = 0; i < n; i++) {
    const uint8_t *b = WiFi.BSSID(i);
    int found = -1;
    for (int j = 0; j < apCount; j++) {
      if (memcmp(aps[j].bssid, b, 6) == 0) { found = j; break; }
    }
    if (found >= 0) {
      String s = WiFi.SSID(i);
      aps[found].hidden = (s.length() == 0);
      strncpy(aps[found].ssid, s.c_str(), 32);
      aps[found].ssid[32] = 0;
      aps[found].rssi = WiFi.RSSI(i);
      aps[found].channel = WiFi.channel(i);
      aps[found].miss = 0;
    } else if (apCount < MAX_APS) {
      ApInfo &a = aps[apCount];
      String s = WiFi.SSID(i);
      a.hidden = (s.length() == 0);
      strncpy(a.ssid, s.c_str(), 32);
      a.ssid[32] = 0;
      memcpy(a.bssid, b, 6);
      a.rssi = WiFi.RSSI(i);
      a.channel = WiFi.channel(i);
      a.miss = 0;
      apCount++;
    }
  }

  // 移除连续 AP_DROP_MISSES 轮未出现的条目（保持其余顺序）
  int w = 0;
  for (int i = 0; i < apCount; i++) {
    if (aps[i].miss < AP_DROP_MISSES) aps[w++] = aps[i];
  }
  if (w != apCount) {
    if (cursor >= w) cursor = w - 1;
    apCount = w;
    listDirty = true;
  }

  // 仅首次扫描按信号排序，之后顺序固定
  if (firstScan && apCount > 1) {
    for (int i = 1; i < apCount; i++) {
      ApInfo key = aps[i];
      int j = i - 1;
      while (j >= 0 && aps[j].rssi < key.rssi) { aps[j + 1] = aps[j]; j--; }
      aps[j + 1] = key;
    }
    listDirty = true;
  }
  firstSortDone = true;
}

// 在“刚完成的扫描结果”里按 BSSID 找目标，返回下标或 -1
static int findTargetInScanResults() {
  int n = WiFi.scanComplete();
  if (n <= 0) return -1;
  int best = -1;
  for (int i = 0; i < n; i++) {
    if (memcmp(WiFi.BSSID(i), targetBssid, 6) == 0 &&
        (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)))
      best = i;
  }
  return best;
}

static void feedTarget(int rawRssi, uint8_t ch) {
  targetRssi = rawRssi;
  targetChannel = ch;
  if (navOn) navVote(rawRssi);
  if (targetRssiEma < -95.0f) targetRssiEma = rawRssi;
  else targetRssiEma += EMA_ALPHA * (rawRssi - targetRssiEma);
  trackMiss = 0;
  targetLost = false;
  pushTrend((int)targetRssiEma);
}

static void pollScan() {
  if (!scanning) {
    if (millis() < nextScanMs) return;
    bool fast = (mode == MODE_TRACK && targetChannel >= 1 &&
                 targetChannel <= 13 &&
                 millis() - lastFullScanMs < TRACK_FULL_SCAN_MS);
    startScan(fast);
    return;
  }
  int16_t r = WiFi.scanComplete();
  if (r == WIFI_SCAN_RUNNING) return;
  if (r != WIFI_SCAN_FAILED) {
    int hit = findTargetInScanResults();
    if (scanningFast) {
      if (hit >= 0) {
        feedTarget(WiFi.RSSI(hit), WiFi.channel(hit));
      } else {
        trackMiss++;
        if (trackMiss >= TRACK_MISS_LIMIT) targetChannel = 0; // 触发全扫重找
      }
    } else {
      if (mode == MODE_TRACK) {
        if (hit >= 0) feedTarget(WiFi.RSSI(hit), WiFi.channel(hit));
        else { targetLost = true; }
      }
      mergeScanIntoRegistry();  // 之后才能 scanDelete
      Serial.printf("[scan] done: %d networks (registry %d)\r\n", r, apCount);
    }
  } else {
    Serial.println("[scan] failed, retry");
  }
  WiFi.scanDelete();
  scanning = false;
  nextScanMs = millis() + ((mode == MODE_TRACK) ? TRACK_RESCAN_DELAY_MS
                                                : LIST_RESCAN_DELAY_MS);
  if (mode == MODE_LIST) drawList();
  updateListHeader();
}

// 可见槽位表：过滤隐藏网络后仍保持顺序稳定
static void rebuildVisible() {
  visCount = 0;
  for (int i = 0; i < apCount && visCount < MAX_APS; i++) {
    if (showHidden || !aps[i].hidden) visIdx[visCount++] = i;
  }
  if (cursor >= visCount) cursor = visCount - 1;
  if (cursor < 0) cursor = 0;
}

// ---------- 列表页 ----------
static void updateListHeader() {
  if (mode != MODE_LIST) return;
  // 上行：scanning 状态
  M5.Lcd.fillRect(150, 2, 88, 10, TFT_NAVY);
  if (scanning) {
    M5.Lcd.setTextFont(1);
    M5.Lcd.setTextColor(TFT_CYAN, TFT_NAVY);
    M5.Lcd.setCursor(150, 2);
    M5.Lcd.print("scanning...");
  }
  // 下行：电量（约每轮扫描读一次即可，这里顺带读）
  int bat = M5.Power.getBatteryLevel();
  if (bat != drawnBat) {
    drawnBat = bat;
    M5.Lcd.fillRect(150, 12, 88, 10, TFT_NAVY);
    M5.Lcd.setTextFont(1);
    M5.Lcd.setTextColor(bat <= 20 ? TFT_RED : (bat <= 40 ? TFT_YELLOW : TFT_GREEN),
                        TFT_NAVY);
    M5.Lcd.setCursor(150, 12);
    if (bat > 0 && bat <= 100) M5.Lcd.printf("BAT %d%%", bat);
    else M5.Lcd.print("BAT --");
  }
}

static void drawListHeaderBase() {
  M5.Lcd.fillRect(0, 0, SCREEN_W, LIST_HEADER_H, TFT_NAVY);
  M5.Lcd.setTextFont(1);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Lcd.setCursor(3, 2);
  M5.Lcd.print("WiFi Signal Hunter");
  M5.Lcd.setTextColor(showHidden ? TFT_DARKGREY : TFT_ORANGE, TFT_NAVY);
  M5.Lcd.setCursor(3, 13);
  M5.Lcd.print(showHidden ? "A-long:hide <hid>" : "hidden OFF  A-long:show");
  drawnBat = -999;
  updateListHeader();
}

static void drawRow(int row, bool force) {
  const ApInfo &a = aps[visIdx[topRow + row]];
  int y = LIST_HEADER_H + row * LIST_ROW_H;
  bool sel = (topRow + row == cursor);
  if (!force && row < drawnRows && drawnRssi[row] == a.rssi &&
      drawnCh[row] == a.channel && drawnCursorRow == (sel ? row : -1) && !sel)
    return; // 无变化且非选中行

  M5.Lcd.fillRect(0, y, SCREEN_W, LIST_ROW_H, TFT_BLACK);
  if (sel) M5.Lcd.fillRect(0, y, SCREEN_W, LIST_ROW_H, TFT_DARKGREY);

  int bars = a.rssi >= -55 ? 5 : a.rssi >= -67 ? 4 : a.rssi >= -75 ? 3
             : a.rssi >= -82 ? 2 : 1;
  for (int b = 0; b < 5; b++) {
    int bh = 3 + b * 2;
    M5.Lcd.fillRect(4 + b * 4, y + LIST_ROW_H - 2 - bh, 3, bh,
                    b < bars ? rssiColor(a.rssi) : TFT_DARKGREY);
  }
  M5.Lcd.setTextFont(1);
  M5.Lcd.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, sel ? TFT_DARKGREY : TFT_BLACK);
  M5.Lcd.setCursor(27, y + 4);
  if (a.hidden)
    M5.Lcd.printf("<hid %02X%02X>", a.bssid[4], a.bssid[5]);
  else
    M5.Lcd.print(a.ssid);
  M5.Lcd.setCursor(150, y + 4);
  M5.Lcd.printf("ch%-2d", a.channel);
  M5.Lcd.setCursor(190, y + 4);
  M5.Lcd.setTextColor(rssiColor(a.rssi), sel ? TFT_DARKGREY : TFT_BLACK);
  M5.Lcd.printf("%4d", a.rssi);

  drawnRssi[row] = a.rssi;
  drawnCh[row] = a.channel;
}

static void drawList() {
  if (mode != MODE_LIST) return;
  rebuildVisible();
  if (listDirty) {
    M5.Lcd.fillScreen(TFT_BLACK);
    drawListHeaderBase();
    drawnRows = 0;
    drawnCursorRow = -1;
    listDirty = false;
  }
  // 滚动窗口跟随光标（可见槽位空间）
  if (cursor < topRow) topRow = cursor;
  if (cursor >= topRow + LIST_ROWS) topRow = cursor - LIST_ROWS + 1;
  int maxTop = visCount > LIST_ROWS ? visCount - LIST_ROWS : 0;
  topRow = clampi(topRow, 0, maxTop);
  bool rowsShifted = (topRow != drawnTop); // 滚动后行内容整体变了
  drawnTop = topRow;

  int rows = visCount - topRow;
  if (rows > LIST_ROWS) rows = LIST_ROWS;
  for (int row = 0; row < rows; row++) {
    const ApInfo &a = aps[visIdx[topRow + row]];
    bool sel = (topRow + row == cursor);
    if (listDirty || rowsShifted || row >= drawnRows ||
        drawnRssi[row] != a.rssi || drawnCh[row] != a.channel) {
      drawRow(row, true);
      if (sel) drawnCursorRow = row;
    } else if (drawnCursorRow == row && !sel) {
      drawRow(row, true); // 取消高亮
      drawnCursorRow = -1;
    } else if (sel && drawnCursorRow != row) {
      drawRow(row, true);
      drawnCursorRow = row;
    }
  }
  for (int row = rows; row < drawnRows; row++) { // 清掉消失的行
    int y = LIST_HEADER_H + row * LIST_ROW_H;
    M5.Lcd.fillRect(0, y, SCREEN_W, LIST_ROW_H, TFT_BLACK);
  }
  drawnRows = rows;
  if (rows == 0) {
    M5.Lcd.setTextFont(2);
    M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    M5.Lcd.setCursor(40, 60);
    M5.Lcd.print(showHidden ? "no networks found" : "no visible networks");
  }
}

// ---------- 追踪页 ----------
static void drawTrackStatic() {
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setTextFont(1);
  M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Lcd.setCursor(4, 4);
  M5.Lcd.print(targetHidden ? "hidden network" : targetName);
  M5.Lcd.setCursor(4, 14);
  M5.Lcd.printf("%02X:%02X:%02X:%02X:%02X:%02X", targetBssid[0],
                targetBssid[1], targetBssid[2], targetBssid[3], targetBssid[4],
                targetBssid[5]);
  M5.Lcd.drawFastHLine(4, 120, 232, TFT_DARKGREY);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(4, 122);
  M5.Lcd.print("-100");
  M5.Lcd.setCursor(216, 122);
  M5.Lcd.print("-30");
  M5.Lcd.setCursor(140, 122);
  M5.Lcd.print("A:scan B:back");
}

static void drawTrackLive() {
  int disp = targetLost ? targetRssi : (int)targetRssiEma;
  M5.Lcd.setTextFont(4);
  M5.Lcd.setTextColor(targetLost ? TFT_DARKGREY : rssiColor(disp), TFT_BLACK);
  M5.Lcd.fillRect(0, 26, 150, 28, TFT_BLACK);
  M5.Lcd.setCursor(4, 26);
  M5.Lcd.printf("%d dBm", disp);

  M5.Lcd.setTextFont(2);
  M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Lcd.fillRect(150, 28, 90, 20, TFT_BLACK);
  M5.Lcd.setCursor(152, 28);
  M5.Lcd.print(targetLost ? "LOST" : rssiWord(disp));
  M5.Lcd.fillRect(190, 14, 50, 10, TFT_BLACK);
  M5.Lcd.setTextFont(1);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(190, 14);
  M5.Lcd.printf("ch%d", targetChannel);

  int pct = clampi((disp + 100) * 100 / 70, 0, 100);
  M5.Lcd.fillRect(4, 58, 232, 10, TFT_DARKGREY);
  M5.Lcd.fillRect(4, 58, 232 * pct / 100, 10,
                  targetLost ? TFT_DARKGREY : rssiColor(disp));

  // 历史曲线：先清曲线区再重画，否则环形缓冲滚动后旧线残留成拖影
  M5.Lcd.fillRect(0, 62, SCREEN_W, 57, TFT_BLACK);
  if (trendLen > 1) {
    for (int i = 1; i < trendLen; i++) {
      int x0 = 4 + (i - 1) * 232 / (TREND_N - 1);
      int x1 = 4 + i * 232 / (TREND_N - 1);
      int v0 = clampi(trend[(trendHead - trendLen + i - 1) % TREND_N], -100, -30);
      int v1 = clampi(trend[(trendHead - trendLen + i) % TREND_N], -100, -30);
      int y0 = 118 - (v0 + 100) * 50 / 70;
      int y1 = 118 - (v1 + 100) * 50 / 70;
      M5.Lcd.drawLine(x0, y0, x1, y1,
                      i == trendLen - 1 ? rssiColor(v1) : TFT_CYAN);
    }
  }
}


// ---------- 3D 导航罗盘：四元数全姿态 + 信号梯度三维方位推算 ----------
// 用法：长按 A 开罗盘（先静止 1s 标定），然后像拿遥控器一样把设备
// "指向"你要走的方向并走两三步，换方向重复。AP 方位按 3D 矢量投票
// 收敛，屏幕显示：水平方位罗盘（0°=正前）+ 仰角梯（楼上/楼下）。
// navOn 在文件前部全局区声明
static float qW = 1, qX = 0, qY = 0, qZ = 0;     // 姿态四元数（设备->世界）
static float upWx = 0, upWy = 0, upWz = 1;       // 世界"上"方向（开局标定）
static float gyroBx = 0, gyroBy = 0, gyroBz = 0; // 陀螺零偏
static float vSumX = 0, vSumY = 0, vSumZ = 0;    // 3D 方位投票矢量和
static float navWeight = 0;                      // 累计证据权重
static float aimAccX = 0, aimAccY = 0, aimAccZ = 0, aimTime = 0; // 走动期间的指向积分
static int navPrevRssi = -200;
static uint32_t navLastMs = 0;
static float navAccEma = 1.0f;                   // |a| 平滑（走动检测）

static void navReset() {
  vSumX = vSumY = vSumZ = 0; navWeight = 0;
  aimAccX = aimAccY = aimAccZ = aimTime = 0;
  navPrevRssi = -200;
}

// 归一化
static void normQ() {
  float n = sqrtf(qW*qW + qX*qX + qY*qY + qZ*qZ);
  qW/=n; qX/=n; qY/=n; qZ/=n;
}

// 设备轴 -> 世界（旋转矩阵作用）
static void devToWorld(float dx, float dy, float dz, float *wx, float *wy, float *wz) {
  // R = 由四元数构造（行向量形式）
  *wx = (1-2*(qY*qY+qZ*qZ))*dx + 2*(qX*qY-qW*qZ)*dy + 2*(qX*qZ+qW*qY)*dz;
  *wy = 2*(qX*qY+qW*qZ)*dx + (1-2*(qX*qX+qZ*qZ))*dy + 2*(qY*qZ-qW*qX)*dz;
  *wz = 2*(qX*qZ-qW*qY)*dx + 2*(qY*qZ+qW*qX)*dy + (1-2*(qX*qX+qY*qY))*dz;
}

// 开局：静止标定陀螺零偏 + 定义世界系（当前指向=X，当前上方=Z）
static void navCalibGyro() {
  float gx, gy, gz, ax, ay, az;
  float sx = 0, sy = 0, sz = 0, sax = 0, say = 0, saz = 0;
  int n = 0;
  for (int i = 0; i < 50; i++) {
    if (M5.Imu.getGyro(&gx, &gy, &gz)) { sx += gx; sy += gy; sz += gz; }
    if (M5.Imu.getAccel(&ax, &ay, &az)) { sax += ax; say += ay; saz += az; }
    n++; delay(20);
  }
  gyroBx = sx/n; gyroBy = sy/n; gyroBz = sz/n;
  // 世界上方向 = 当前加速度方向（静止时指向天）
  float m = sqrtf(sax*sax + say*say + saz*saz) / n;
  upWx = sax/n/m; upWy = say/n/m; upWz = saz/n/m;
  qW = 1; qX = qY = qZ = 0; // 世界系=当前设备系，四元数从恒等开始
  navAccEma = 1.0f;
  Serial.printf("[nav] calib gyro=(%.1f,%.1f,%.1f) up=(%.2f,%.2f,%.2f)\r\n",
                gyroBx, gyroBy, gyroBz, upWx, upWy, upWz);
}

// 每 20ms：四元数积分 + 加计姿态修正 + 走动期间的指向累计
static void navTick() {
  float gx, gy, gz, ax, ay, az;
  if (!M5.Imu.getGyro(&gx, &gy, &gz)) return;
  M5.Imu.getAccel(&ax, &ay, &az);
  uint32_t now = millis();
  float dt = (navLastMs == 0) ? 0 : (now - navLastMs) / 1000.0f;
  navLastMs = now;
  if (dt <= 0 || dt > 0.2f) return;

  // 陀螺（deg/s -> rad/s，去零偏）
  float wx = (gx - gyroBx) * 0.017453f;
  float wy = (gy - gyroBy) * 0.017453f;
  float wz = (gz - gyroBz) * 0.017453f;

  // 加计修正：预测重力(世界 -up 在设备系) 与实测加速度的叉积误差
  float am = sqrtf(ax*ax + ay*ay + az*az);
  if (am > 0.5f && am < 1.5f) {
    // 世界 -up 变换回设备系 = R^T * (-up)
    // R^T 的行 = R 的列；直接用 devToWorld 的转置形式
    float px = -( (1-2*(qY*qY+qZ*qZ))*upWx + 2*(qX*qY+qW*qZ)*upWy + 2*(qX*qZ-qW*qY)*upWz );
    float py = -( 2*(qX*qY-qW*qZ)*upWx + (1-2*(qX*qX+qZ*qZ))*upWy + 2*(qY*qZ+qW*qX)*upWz );
    float pz = -( 2*(qX*qZ+qW*qY)*upWx + 2*(qY*qZ-qW*qX)*upWy + (1-2*(qX*qX+qY*qY))*upWz );
    float ex = (ay/am)*pz - (az/am)*py;
    float ey = (az/am)*px - (ax/am)*pz;
    float ez = (ax/am)*py - (ay/am)*px;
    float kp = 0.06f;
    wx += kp * ex; wy += kp * ey; wz += kp * ez;
  }

  // 四元数积分 q += 0.5 * q ⊗ (0,wx,wy,wz) * dt
  float dw = 0.5f * (-qX*wx - qY*wy - qZ*wz) * dt;
  float dx = 0.5f * ( qW*wx + qY*wz - qZ*wy) * dt;
  float dy = 0.5f * ( qW*wy - qX*wz + qZ*wx) * dt;
  float dz = 0.5f * ( qW*wz + qX*wy - qY*wx) * dt;
  qW += dw; qX += dx; qY += dy; qZ += dz;
  normQ();

  // 走动检测：|a| 偏离 1g（平滑）
  navAccEma += 0.1f * (am - navAccEma);
  if (fabsf(navAccEma - 1.0f) > 0.06f) {
    // 走动中：累计当前指向（设备长轴 X 的世界方向）
    float axw, ayw, azw;
    devToWorld(1, 0, 0, &axw, &ayw, &azw);
    aimAccX += axw * dt; aimAccY += ayw * dt; aimAccZ += azw * dt;
    aimTime += dt;
  }
}

// 每次快扫出结果：3D 梯度投票
static void navVote(int rssi) {
  if (navPrevRssi < -100) { navPrevRssi = rssi; return; }
  float d = rssi - navPrevRssi;
  navPrevRssi = rssi;
  if (fabsf(d) < 2.0f) return;
  if (aimTime < 0.4f) return; // 期间基本没走动
  float m = sqrtf(aimAccX*aimAccX + aimAccY*aimAccY + aimAccZ*aimAccZ);
  if (m < 1e-4f) return;
  float w = fabsf(d) > 10 ? 10 : fabsf(d);
  float s = (d > 0 ? 1.0f : -1.0f) * w / m; // 变强=沿走向；变弱=反向
  vSumX += aimAccX * s; vSumY += aimAccY * s; vSumZ += aimAccZ * s;
  navWeight += w;
  aimAccX = aimAccY = aimAccZ = aimTime = 0;
  Serial.printf("[nav] d=%.0f w=%.0f total=%.0f v=(%.2f,%.2f,%.2f)\r\n",
                d, w, navWeight, vSumX, vSumY, vSumZ);
}

// 屏幕：水平方位罗盘 + 仰角梯
static void drawNav() {
  M5.Lcd.fillRect(0, 62, SCREEN_W, 57, TFT_BLACK);
  int cy = 92;
  // 左侧：水平方位环（0°=正前）
  int cx = 58, r = 24;
  M5.Lcd.drawCircle(cx, cy, r, TFT_DARKGREY);
  M5.Lcd.drawFastHLine(cx - 4, cy, 8, TFT_DARKGREY); // 正前刻度
  float conf = navWeight > 30 ? 1.0f : navWeight / 30.0f;
  if (navWeight > 3) {
    float m = sqrtf(vSumX*vSumX + vSumY*vSumY + vSumZ*vSumZ);
    float dX = vSumX/m, dY = vSumY/m, dZ = vSumZ/m;
    // 当前指向的水平投影与 AP 方向水平投影的夹角（绕世界 up）
    float axw, ayw, azw;
    devToWorld(1, 0, 0, &axw, &ayw, &azw); // 当前 aim
    // 水平化
    float ad = axw*upWx + ayw*upWy + azw*upWz;
    float ahx = axw - ad*upWx, ahy = ayw - ad*upWy, ahz = azw - ad*upWz;
    float bd = dX*upWx + dY*upWy + dZ*upWz;
    float bhx = dX - bd*upWx, bhy = dY - bd*upWy, bhz = dZ - bd*upWz;
    float ahm = sqrtf(ahx*ahx + ahy*ahy + ahz*ahz);
    float bhm = sqrtf(bhx*bhx + bhy*bhy + bhz*bhz);
    if (ahm > 1e-3f && bhm > 1e-3f) {
      float cosA = (ahx*bhx + ahy*bhy + ahz*bhz) / (ahm * bhm);
      float sinA = ( (ahy*bhz - ahz*bhy)*upWx + (ahz*bhx - ahx*bhz)*upWy
                   + (ahx*bhy - ahy*bhx)*upWz ) / (ahm * bhm);
      float yawErr = atan2f(sinA, cosA); // 正=右转
      uint16_t col = conf > 0.7f ? TFT_GREEN : TFT_CYAN;
      float px = sinf(yawErr), py = -cosf(yawErr); // 0=上(正前)
      M5.Lcd.drawLine(cx - px*r*0.5f, cy - py*r*0.5f,
                      cx + px*r*0.9f, cy + py*r*0.9f, col);
      M5.Lcd.fillTriangle(cx + px*r*0.95f, cy + py*r*0.95f,
                          cx + px*r*0.55f - py*r*0.2f, cy + py*r*0.55f + px*r*0.2f,
                          cx + px*r*0.55f + py*r*0.2f, cy + py*r*0.55f - px*r*0.2f, col);
      // 环下方的角度文字
      M5.Lcd.setTextFont(1);
      M5.Lcd.setTextColor(col, TFT_BLACK);
      M5.Lcd.setCursor(cx - 18, cy + r + 4);
      int deg = (int)(yawErr * 57.29578f);
      M5.Lcd.printf("%+d\xE7", deg);
    }
    // 右侧：仰角梯（AP 在楼上/楼下）
    float elev = asinf(fmaxf(-1.0f, fminf(1.0f, bd))) * 57.29578f;
    int ex = 150;
    M5.Lcd.drawFastVLine(ex, cy - 22, 44, TFT_DARKGREY);
    M5.Lcd.drawFastHLine(ex - 4, cy, 8, TFT_DARKGREY); // 0°
    int ey = cy - (int)(elev / 45.0f * 22.0f);
    uint16_t ecol = conf > 0.7f ? TFT_GREEN : TFT_CYAN;
    M5.Lcd.fillCircle(ex, ey, 4, ecol);
    M5.Lcd.setTextFont(1);
    M5.Lcd.setTextColor(ecol, TFT_BLACK);
    M5.Lcd.setCursor(ex + 10, ey - 3);
    M5.Lcd.printf("%+.0f\xE7", elev);
    M5.Lcd.setCursor(ex + 10, cy + 10);
    if (elev > 12) M5.Lcd.print("^楼上方向");
    else if (elev < -12) M5.Lcd.print("v楼下方向");
    else M5.Lcd.print("同层");
  } else {
    M5.Lcd.setFont(&fonts::efontCN_12);
    M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Lcd.setCursor(40, cy - 8);
    M5.Lcd.print("指向前进方向走几步");
  }
  // 置信度条
  M5.Lcd.fillRect(4, 118, (int)(conf * 100), 4, conf > 0.7f ? TFT_GREEN : TFT_CYAN);
  M5.Lcd.drawRect(4, 118, 100, 4, TFT_DARKGREY);
}

static void geigerBeep() {
  if (targetLost || mode != MODE_TRACK) return;
  if (millis() < nextBeepMs) return;
  int disp = (int)targetRssiEma;
  int interval = clampi(map(disp, -90, -40, 1000, 70), 70, 1000);
  M5.Speaker.tone(2350, 24);
  nextBeepMs = millis() + interval;
}

// 重力翻转：横屏两个方向之间切换
static void checkRotation() {
  float ax, ay, az;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) return;
  float g = AX_SIGN * ax;
  uint8_t want;
  if (g < -ROT_THRESHOLD_G)      want = 3;
  else if (g > ROT_THRESHOLD_G)  want = 1;
  else return;
  if (want != rotCandidate) {
    rotCandidate = want;
    rotCandidateSince = millis();
    return;
  }
  if (want != rotCur && millis() - rotCandidateSince > ROT_STABLE_MS) {
    rotCur = want;
    M5.Lcd.setRotation(rotCur);
    Serial.printf("[imu] ax=%.2f ay=%.2f az=%.2f -> rotation %d\r\n",
                  ax, ay, az, rotCur);
    if (mode == MODE_LIST) { listDirty = true; drawList(); }
    else { drawTrackStatic(); drawTrackLive(); }
  }
}

// ---------- Arduino 生命周期 ----------
void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = true;
  M5.begin(cfg);
  M5.Lcd.setRotation(rotCur);
  M5.Lcd.setBrightness(90);
  M5.Speaker.setVolume(200);

  Serial.begin(115200);
  Serial.printf("\r\n=== WiFi Signal Hunter for M5StickS3 v4 ===\r\n");
  Serial.printf("board: %s\r\n",
                M5.getBoard() == m5gfx::board_t::board_M5StickS3
                    ? "M5StickS3 (autodetected)"
                    : "WARNING: not detected as M5StickS3");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.persistent(false);

  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setTextFont(2);
  M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Lcd.setCursor(30, 50);
  M5.Lcd.print("WiFi Signal Hunter");
  M5.Lcd.setTextFont(1);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(52, 75);
  M5.Lcd.print("scanning networks...");

  startScan(false);
}

void loop() {
  M5.update();
  pollScan();

  if (millis() >= nextRotCheckMs) {
    nextRotCheckMs = millis() + 150;
    checkRotation();
  }

  if (mode == MODE_LIST) {
    // 长按 A：切换显示/隐藏“隐藏 SSID”信号源
    if (M5.BtnA.pressedFor(600)) {
      if (!hiddenToggleLatch) {
        hiddenToggleLatch = true;
        showHidden = !showHidden;
        rebuildVisible();
        topRow = 0;
        listDirty = true;
        Serial.printf("[ui] showHidden=%d\r\n", showHidden);
        drawList();
      }
    }
    if (M5.BtnA.wasReleased()) hiddenToggleLatch = false;

    if (M5.BtnA.wasClicked() && visCount > 0) {
      drawList();
      cursor = (cursor + 1) % visCount;
      drawList();
    }
    if (M5.BtnB.wasClicked() && visCount > 0) {
      const ApInfo &a = aps[visIdx[cursor]];
      memcpy(targetBssid, a.bssid, 6);
      strncpy(targetName, a.hidden ? "" : a.ssid, 32);
      targetName[32] = 0;
      targetHidden = a.hidden;
      targetRssi = a.rssi;
      targetRssiEma = a.rssi;
      targetChannel = a.channel;
      trendLen = 0;
      trendHead = 0;
      pushTrend(targetRssi);
      targetLost = false;
      trackMiss = 0;
      lastFullScanMs = millis();
      mode = MODE_TRACK;
      Serial.printf("[mode] TRACK '%s' bssid=%02x:%02x:%02x:%02x:%02x:%02x ch%d\r\n",
                    targetHidden ? "<hidden>" : targetName,
                    targetBssid[0], targetBssid[1], targetBssid[2],
                    targetBssid[3], targetBssid[4], targetBssid[5],
                    targetChannel);
      drawTrackStatic();
      drawTrackLive();
    }
  } else { // MODE_TRACK
    if (navOn) navTick();
    if (M5.BtnB.wasClicked()) {
      mode = MODE_LIST;
      targetChannel = 0;
      navOn = false;
      listDirty = true;
      Serial.println("[mode] LIST");
      drawList();
    }
    // B 长按：重置方位投票；A 长按：开关罗盘
    if (M5.BtnB.pressedFor(800)) {
      navReset();
      drawTrackLive();
      M5.Speaker.tone(1500, 60);
      while (M5.BtnB.isPressed()) { M5.update(); delay(10); }
    }
    if (M5.BtnA.pressedFor(800)) {
      navOn = !navOn;
      if (navOn) { navReset(); navCalibGyro(); }
      drawTrackStatic();
      drawTrackLive();
      M5.Speaker.tone(navOn ? 2000 : 1200, 60);
      while (M5.BtnA.isPressed()) { M5.update(); delay(10); }
    }
    if (M5.BtnA.wasClicked()) {
      trackMiss = 0;
      startScan(false);
    }
    if (millis() - lastSampleMs > 200) {
      lastSampleMs = millis();
      drawTrackLive();
      if (navOn) drawNav();
    }
    geigerBeep();
  }

  delay(10);
}
