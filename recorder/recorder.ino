/*
 * Voice Recorder — M5StickS3 离线录音笔
 *
 * A 单击 开始/停止录音（8kHz µ-law WAV，4:1 压缩，本地 LittleFS）
 * 录音页实时显示 已录时长 + 剩余可录时长（按剩余空间换算）
 * 每次录音结束自动尝试连家里 WiFi（8 秒超时），连上即把所有本地录音
 * 上传到 Cloudflare R2 并【删除本地文件】释放空间；B 键手动触发上传。
 * A 长按深度睡眠，单击 A/B 唤醒。3 分钟无操作自动睡。
 *
 * 容量：5.6MB 存储 ÷ 8KB/s ≈ 11 分 40 秒（清空后恢复满额）
 */

#include <M5Unified.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <vector>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define SECRETS_MISSING 1
#include "secrets.example.h"
#endif

static const uint32_t MIC_RATE = 16000;
static const size_t CHUNK = 1024;            // 64ms @16k
static const uint32_t BYTES_PER_SEC = 8000;  // 8kHz µ-law
static const uint32_t AUTO_SLEEP_MS = 3UL * 60 * 1000;

static int16_t chunkBuf[CHUNK];
static File recFile;
static size_t recBytes = 0;
static char recName[32] = {0};
static uint32_t recStartMs = 0;
static volatile bool recActive = false;
static float recLevel = 0;
static uint32_t lastActivityMs = 0;

// ---------- µ-law ----------
static uint8_t pcm2ul(int32_t s) {
  uint8_t sign = (s < 0) ? 0x80 : 0;
  if (s < 0) s = -s;
  s += 132;
  if (s > 32635) s = 32635;
  uint8_t e = 0;
  for (int32_t t = s >> 7; t; t >>= 1) e++;
  uint8_t m = (s >> (e + 3)) & 0x0F;
  return ~(uint8_t)(sign | (e << 4) | m);
}

static void buildWavHeader(uint8_t *h, uint32_t dataLen) {
  uint32_t u32; uint16_t u16;
  memcpy(h + 0, "RIFF", 4);
  u32 = 36 + dataLen; memcpy(h + 4, &u32, 4);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);
  u32 = 16; memcpy(h + 16, &u32, 4);
  u16 = 7; memcpy(h + 20, &u16, 2);          // µ-law
  u16 = 1; memcpy(h + 22, &u16, 2);          // mono
  u32 = 8000; memcpy(h + 24, &u32, 4);
  u32 = 8000; memcpy(h + 28, &u32, 4);
  u16 = 1; memcpy(h + 32, &u16, 2);
  u16 = 8; memcpy(h + 34, &u16, 2);
  memcpy(h + 36, "data", 4);
  u32 = dataLen; memcpy(h + 40, &u32, 4);
}

// 录音回调：16k PCM -> 2:1 平均降采样 -> µ-law -> 落盘
static void recWrite(void *, void *data, size_t samples) {
  static uint8_t ubuf[CHUNK / 2];
  if (!recFile || !recActive) return;
  const int16_t *p = (const int16_t *)data;
  size_t n = samples / 2;
  int32_t peak = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t s = (p[i * 2] + p[i * 2 + 1]) / 2;
    ubuf[i] = pcm2ul(s);
    int32_t a = s < 0 ? -s : s;
    if (a > peak) peak = a;
  }
  recLevel = 0.5f * recLevel + 0.5f * (peak / 12000.0f);
  recBytes += recFile.write(ubuf, n);
}

// ---------- UI ----------
static uint32_t remainSec() {
  int64_t freeB = (int64_t)LittleFS.totalBytes() - LittleFS.usedBytes();
  if (freeB < 0) freeB = 0;
  return freeB / BYTES_PER_SEC;
}
static int pendingCount() {
  File root = LittleFS.open("/");
  File f; int n = 0;
  while ((f = root.openNextFile())) {
    if (!f.isDirectory() && String(f.name()).endsWith(".wav")) n++;
    f.close();
  }
  root.close();
  return n;
}
static void fmtMMSS(uint32_t s, char *buf, size_t n) {
  snprintf(buf, n, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void drawIdle() {
  M5.Lcd.fillScreen(TFT_BLACK);
  char t[8];
  fmtMMSS(remainSec(), t, sizeof(t));
  uint16_t col = TFT_GREEN;
  if (remainSec() < 60) col = TFT_RED;
  else if (remainSec() < 180) col = TFT_ORANGE;
  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(10, 6);
  M5.Lcd.print("录音笔 · 剩余可录");
  M5.Lcd.setFont(&fonts::Font7);
  M5.Lcd.setTextColor(col, TFT_BLACK);
  M5.Lcd.setCursor(20, 28);
  M5.Lcd.print(t);
  M5.Lcd.setFont(&fonts::efontCN_12);
  char buf[32];
  int pend = pendingCount();
  snprintf(buf, sizeof(buf), "待上传 %d 段", pend);
  M5.Lcd.setTextColor(pend ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(10, 100);
  M5.Lcd.print(buf);
  int bat = M5.Power.getBatteryLevel();
  if (bat > 0) {
    snprintf(buf, sizeof(buf), "电%d%%", bat);
    M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Lcd.setCursor(190, 100);
    M5.Lcd.print(buf);
  }
  M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Lcd.setCursor(10, 118);
  M5.Lcd.print("A:录音/停止 B:上传 B长按:关机");
}

static void drawRec(uint32_t now) {
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.fillCircle(14, 14, 7, TFT_RED);
  M5.Lcd.setFont(&fonts::efontCN_12);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  char buf[24];
  uint32_t el = (now - recStartMs) / 1000;
  snprintf(buf, sizeof(buf), "已录 %02u:%02u", (unsigned)(el / 60), (unsigned)(el % 60));
  M5.Lcd.setCursor(34, 8);
  M5.Lcd.print(buf);
  uint32_t rem = remainSec();
  fmtMMSS(rem, buf, sizeof(buf));
  snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " 可录");
  M5.Lcd.setTextColor(rem < 60 ? TFT_RED : TFT_ORANGE, TFT_BLACK);
  M5.Lcd.setCursor(34, 30);
  M5.Lcd.print(buf);
  int bw = (int)(220 * recLevel);
  M5.Lcd.fillRect(10, 60, 220, 18, TFT_DARKGREY);
  M5.Lcd.fillRect(10, 60, bw, 18, recLevel > 0.7f ? TFT_RED : TFT_GREEN);
  M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(10, 118);
  M5.Lcd.print("按A停止并保存");
}

static void drawMsg(const char *l1, const char *l2, uint16_t c) {
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setFont(&fonts::efontCN_16_b);
  M5.Lcd.setTextColor(c, TFT_BLACK);
  M5.Lcd.setCursor(30, 40);
  M5.Lcd.print(l1);
  if (l2 && l2[0]) {
    M5.Lcd.setFont(&fonts::efontCN_12);
    M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    M5.Lcd.setCursor(30, 75);
    M5.Lcd.print(l2);
  }
}

// ---------- 录音 ----------
static void startRecording() {
  Preferences prefs;
  prefs.begin("recdr", false);
  uint32_t seq = prefs.getUInt("seq", 0) + 1;
  prefs.putUInt("seq", seq);
  prefs.end();
  snprintf(recName, sizeof(recName), "/R%05u.wav", (unsigned)seq);
  recFile = LittleFS.open(recName, FILE_WRITE);
  if (!recFile) { drawMsg("存储错误", recName, TFT_RED); delay(2000); return; }
  uint8_t hdr[44];
  buildWavHeader(hdr, 0);
  recFile.write(hdr, 44);
  recBytes = 0;
  recStartMs = millis();
  recLevel = 0;
  recActive = true;
  M5.Speaker.end();
  M5.Mic.setBufferReleaseCallback(nullptr, recWrite);
  M5.Mic.begin();
  Serial.printf("[rec] %s start\r\n", recName);
}

static void stopRecording() {
  recActive = false;
  while (M5.Mic.isRecording()) M5.delay(1);
  M5.Mic.end();
  if (recFile) {
    uint8_t hdr[44];
    buildWavHeader(hdr, recBytes);
    recFile.seek(0);
    recFile.write(hdr, 44);
    recFile.close();
  }
  Serial.printf("[rec] %s %uB (%.0fs)\r\n", recName, (unsigned)recBytes,
                recBytes / (float)BYTES_PER_SEC);
}

// ---------- WiFi + R2 上传 ----------
static String urlEnc(const char *s) {
  String o;
  for (const char *p = s; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { char b[4]; snprintf(b, 4, "%%%02X", c); o += b; }
  }
  return o;
}
static void hmacSha256(const uint8_t *key, size_t klen, const char *data,
                       uint8_t out[32]) {
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                  key, klen, (const uint8_t *)data, strlen(data), out);
}

// 上传单个文件；成功返回空串
static String r2Put(const char *file) {
  File f = LittleFS.open(file, "r");
  if (!f) return "open";
  size_t fsize = f.size();
  time_t now = time(nullptr);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  char ts[20], date[9];
  strftime(ts, sizeof(ts), "%Y%m%dT%H%M%SZ", &tmv);
  strftime(date, sizeof(date), "%Y%m%d", &tmv);
  String key = String("/") + R2_BUCKET + "/" + R2_PREFIX + (file + 1);
  String ph = "UNSIGNED-PAYLOAD";
  String canonical = "PUT\n" + urlEnc(key.c_str()) + "\n\n"
      "host:" + R2_HOST + "\n"
      "x-amz-content-sha256:" + ph + "\n"
      "x-amz-date:" + ts + "\n\n"
      "host;x-amz-content-sha256;x-amz-date\n" + ph;
  uint8_t ch[32];
  mbedtls_sha256((const uint8_t *)canonical.c_str(), canonical.length(), ch, 0);
  char chex[65];
  for (int i = 0; i < 32; i++) sprintf(chex + i * 2, "%02x", ch[i]);
  String sts = "AWS4-HMAC-SHA256\n" + String(ts) + "\n" + date +
               "/auto/s3/aws4_request\n" + chex;
  uint8_t kD[32], kR[32], kS[32], kF[32];
  {
    uint8_t k0[64];
    String ks = "AWS4" + String(R2_SECRET);
    memcpy(k0, ks.c_str(), ks.length());
    hmacSha256(k0, ks.length(), date, kD);
    hmacSha256(kD, 32, "auto", kR);
    hmacSha256(kR, 32, "s3", kS);
    hmacSha256(kS, 32, "aws4_request", kF);
  }
  uint8_t sig[32];
  hmacSha256(kF, 32, sts.c_str(), sig);
  char sh[65];
  for (int i = 0; i < 32; i++) sprintf(sh + i * 2, "%02x", sig[i]);
  String auth = "AWS4-HMAC-SHA256 Credential=" + String(R2_ACCESS_KEY) + "/" +
                date + "/auto/s3/aws4_request, "
                "SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature=" +
                sh;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(30);
  if (!client.connect(R2_HOST, 443)) { f.close(); return "TCP"; }
  client.printf("PUT %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "Authorization: %s\r\n"
                "x-amz-content-sha256: %s\r\n"
                "x-amz-date: %s\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n",
                key.c_str(), R2_HOST, auth.c_str(), ph.c_str(), ts, (int)fsize);
  uint8_t buf[8192];
  size_t sent = 0;
  while (sent < fsize) {
    size_t n = f.read(buf, sizeof(buf));
    if (!n) break;
    size_t off = 0;
    while (off < n) {
      size_t w = client.write(buf + off, n - off);
      if (w == 0) delay(2);
      off += w;
    }
    sent += n;
  }
  f.close();
  uint32_t t0 = millis();
  while (!client.available() && client.connected() && millis() - t0 < 30000) delay(10);
  String line = client.readStringUntil('\n');
  client.stop();
  Serial.printf("[r2] %s %s", file, line.c_str());
  return line.substring(9, 12) == "200" ? "" : line.substring(0, 30);
}

// 连 WiFi + 上传全部 + 删除
static void uploadAll(bool manual) {
  if (pendingCount() == 0) {
    if (manual) { drawMsg("没有待上传", "先录一段吧", TFT_DARKGREY); delay(1500); }
    return;
  }
  drawMsg("连接 WiFi ...", WIFI_SSID, TFT_CYAN);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) M5.delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_OFF);
    if (manual) { drawMsg("WiFi 未连上", "稍后再试", TFT_ORANGE); delay(1500); }
    else Serial.println("[up] wifi fail, keep local");
    drawIdle();
    return;
  }
  configTime(0, 0, "ntp1.aliyun.com", "pool.ntp.org");
  struct tm tnow;
  getLocalTime(&tnow, 5000);

  std::vector<String> names;
  File root = LittleFS.open("/");
  File f;
  while ((f = root.openNextFile())) {
    String n = f.name();
    if (!f.isDirectory() && n.endsWith(".wav")) names.push_back(n);
    f.close();
  }
  root.close();
  int ok = 0;
  for (size_t i = 0; i < names.size(); i++) {
    char l1[24];
    snprintf(l1, sizeof(l1), "上传 %d/%d", (int)i + 1, (int)names.size());
    drawMsg(l1, names[i].c_str(), TFT_CYAN);
    String err = r2Put(names[i].c_str());
    if (err.isEmpty()) {
      LittleFS.remove(names[i]);
      ok++;
      Serial.printf("[up] OK+del %s\r\n", names[i].c_str());
    } else {
      Serial.printf("[up] FAIL %s %s\r\n", names[i].c_str(), err.c_str());
    }
  }
  WiFi.mode(WIFI_OFF);
  char l2[40];
  snprintf(l2, sizeof(l2), "成功 %d 段，本地已清空", ok);
  drawMsg(ok == (int)names.size() ? "全部上传完成" : "部分失败，保留重试",
          l2, ok == (int)names.size() ? TFT_GREEN : TFT_ORANGE);
  delay(1500);
  drawIdle();
}

// ---------- 睡眠 ----------
static void goSleep() {
  if (recActive) stopRecording();
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.sleep();
  esp_sleep_enable_ext1_wakeup((1ULL << 11) | (1ULL << 12),
                               ESP_EXT1_WAKEUP_ANY_LOW);
  M5.Power.deepSleep(m5::Power_Class::sleep_no_timer);
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_mic = true;
  cfg.internal_spk = false;
  M5.begin(cfg);
  M5.Lcd.setRotation(3);
  M5.Lcd.setBrightness(90);
  Serial.begin(115200);
  Serial.printf("\r\n=== Voice Recorder (ulaw) ===\r\n");
  if (!LittleFS.begin(true)) {
    drawMsg("存储初始化失败", "", TFT_RED);
    delay(3000);
    goSleep();
    return;
  }
  lastActivityMs = millis();
  drawIdle();
}

void loop() {
  M5.update();
  uint32_t now = millis();

  // B 长按关机（录音中也不误触：录音中忽略关机，先松 A 完成录音）
  if (!recActive && M5.BtnB.pressedFor(1200)) { goSleep(); return; }

  if (!recActive) {
    // 单击 A 开始录音
    if (M5.BtnA.wasClicked()) {
      if (remainSec() < 10) {
        drawMsg("空间不足", "B 键上传清空", TFT_RED);
        delay(1500);
      } else {
        startRecording();
        if (recActive) drawRec(now);
      }
      lastActivityMs = now;
    }
    if (M5.BtnB.wasClicked()) {
      uploadAll(true);
      lastActivityMs = now;
    }
    if (now - lastActivityMs > AUTO_SLEEP_MS) { goSleep(); return; }
  } else {
    // 录音中：再单击 A 停止（起步 1.5s 保护期，吞掉开始键的残留事件）
    if (M5.BtnA.wasClicked() && millis() - recStartMs > 1500) {
      stopRecording();
      lastActivityMs = now;
      if (recBytes < BYTES_PER_SEC) { // <1 秒视为误触，丢弃
        LittleFS.remove(recName);
        drawMsg("太短，已丢弃", "按A重新录", TFT_ORANGE);
        delay(1200);
      } else {
#ifdef SECRETS_MISSING
        drawMsg("录音已保存", "未配置上传密钥", TFT_ORANGE);
        delay(1500);
#else
        // 自动尝试上传（家里 WiFi 可达即自动，8 秒内连不上就跳过）
        uploadAll(false);
#endif
      }
      drawIdle();
    } else {
      M5.Mic.record(chunkBuf, CHUNK, MIC_RATE);
      if (now - lastActivityMs > 250) {
        lastActivityMs = now;
        drawRec(now);
      }
      // 空间保护
      if (remainSec() < 5) {
        stopRecording();
        drawMsg("存储已满", "B 键上传清空", TFT_ORANGE);
        delay(1500);
        drawIdle();
      }
    }
  }
  delay(2);
}
