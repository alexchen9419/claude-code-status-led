// Claude Code 狀態燈 v3 - ESP32-S3 + 板載 WS2812
// 重點事件會「鎖存」並持續循環播放，直到你確認（按 BOOT 鍵或送出下一個 prompt）
// 需要函式庫：Adafruit NeoPixel
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Adafruit_NeoPixel.h>

#include "secrets.h"   // 複製 secrets.example.h 為 secrets.h 並填入 WiFi 帳密

#define LED_PIN    48   // DevKitC-1 v1.0 = 48，v1.1 = 38
#define LED_COUNT  1
#define ACK_BUTTON 0    // 板上的 BOOT 鍵，按下 = 已讀

Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
WebServer server(80);

// ================= 基礎狀態（持續循環）=================
enum State { IDLE, WORKING, APPROVAL, DONE, ERR, COMPACT };
State state = IDLE;
int subagents = 0;

void setState(State s) { state = s; }

// ================= 鎖存事件（循環播放直到確認）=================
struct Latch { const char* id; uint32_t color; uint8_t count; };
Latch latches[] = {
  { "fail",   0xFF4000, 0 },   // 工具執行失敗：橘紅
  { "denied", 0xFF0000, 0 },   // 權限被拒：紅
  { "task",   0x00FF00, 0 },   // 任務完成：綠
};
const int LATCH_N = sizeof(latches) / sizeof(latches[0]);
unsigned long cycleAnchor = 0;

const uint16_t BASE_MS  = 2500;  // 每輪先顯示基礎狀態多久
const uint16_t BLINK_MS = 150;   // 單次閃爍亮/暗時間
const uint16_t GAP_MS   = 500;   // 不同事件之間的間隔
const uint8_t  MAX_SHOW = 5;     // 次數最多顯示 5 下

bool anyLatched() {
  for (int i = 0; i < LATCH_N; i++) if (latches[i].count) return true;
  return false;
}

void ack() {
  for (int i = 0; i < LATCH_N; i++) latches[i].count = 0;
}

// ================= 一次性閃爍（不鎖存）=================
uint32_t flashColor = 0;
uint16_t flashMs = 0;
uint8_t  flashTimes = 0;
unsigned long flashStart = 0;

void startFlash(uint32_t rgb, uint16_t ms, uint8_t n) {
  flashColor = rgb; flashMs = ms; flashTimes = n; flashStart = millis();
}

// ================= 繪製 =================
void fillRGB(uint32_t c) {
  for (int i = 0; i < LED_COUNT; i++)
    led.setPixelColor(i, led.Color((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF));
  led.show();
}
void fill(uint8_t r, uint8_t g, uint8_t b) { fillRGB(((uint32_t)r << 16) | ((uint32_t)g << 8) | b); }

bool renderFlash() {
  if (flashTimes == 0) return false;
  unsigned long t = millis() - flashStart;
  unsigned long cycle = (unsigned long)flashMs * 2;
  if (t >= cycle * flashTimes) { flashTimes = 0; return false; }
  fillRGB((t % cycle) < flashMs ? flashColor : 0);
  return true;
}

void renderBase(unsigned long now) {
  switch (state) {
    case IDLE:
      fill(3, 3, 3);
      break;
    case WORKING: {
      if (subagents > 0) {
        float k = (sin(now / (400.0 / min(subagents, 4))) + 1.0) / 2.0;
        fill((uint8_t)(40 + 140 * k), 0, (uint8_t)(60 + 195 * k));
      } else {
        float k = (sin(now / 400.0) + 1.0) / 2.0;
        fill(0, (uint8_t)(20 * k), (uint8_t)(30 + 225 * k));
      }
      break;
    }
    case APPROVAL:  // 琥珀快閃，一直閃到你處理
      if ((now / 300) % 2) fill(255, 120, 0); else fill(0, 0, 0);
      break;
    case DONE: {    // 綠色慢呼吸，一直持續到下一個 prompt
      float k = (sin(now / 900.0) + 1.0) / 2.0;
      fill(0, (uint8_t)(40 + 215 * k), 0);
      break;
    }
    case ERR:
      if ((now / 700) % 2) fill(255, 0, 0); else fill(0, 0, 0);
      break;
    case COMPACT: {
      float k = (sin(now / 120.0) + 1.0) / 2.0;
      fill(0, (uint8_t)(60 + 195 * k), (uint8_t)(60 + 195 * k));
      break;
    }
  }
}

// 每輪：先播放所有鎖存事件 → 再顯示基礎狀態 BASE_MS → 重複
bool renderLatches(unsigned long now) {
  if (!anyLatched()) return false;

  unsigned long overlay = 0;
  for (int i = 0; i < LATCH_N; i++)
    if (latches[i].count)
      overlay += (unsigned long)min(latches[i].count, MAX_SHOW) * BLINK_MS * 2 + GAP_MS;

  unsigned long pos = (now - cycleAnchor) % (overlay + BASE_MS);
  if (pos >= overlay) return false;  // 這段時間顯示基礎狀態

  for (int i = 0; i < LATCH_N; i++) {
    if (!latches[i].count) continue;
    unsigned long seg = (unsigned long)min(latches[i].count, MAX_SHOW) * BLINK_MS * 2;
    if (pos < seg) {
      fillRGB((pos % (BLINK_MS * 2)) < BLINK_MS ? latches[i].color : 0);
      return true;
    }
    pos -= seg;
    if (pos < GAP_MS) { fill(0, 0, 0); return true; }
    pos -= GAP_MS;
  }
  return false;
}

void render() {
  unsigned long now = millis();
  if (renderFlash()) return;
  if (renderLatches(now)) return;
  renderBase(now);
}

// ================= 按鈕 =================
void checkButton() {
  static bool last = HIGH;
  static unsigned long lastChange = 0;
  bool cur = digitalRead(ACK_BUTTON);
  if (cur != last && millis() - lastChange > 40) {
    lastChange = millis();
    last = cur;
    if (cur == LOW) {           // 按下
      ack();
      if (state == DONE || state == ERR) setState(IDLE);
      startFlash(0xFFFFFF, 80, 1);
    }
  }
}

// ================= HTTP =================
void ok() { server.send(200, "text/plain", "ok"); }

uint32_t parseHex(const String& s, uint32_t def) {
  if (s.length() != 6) return def;
  return (uint32_t)strtoul(s.c_str(), nullptr, 16);
}

void setup() {
  Serial.begin(115200);
  pinMode(ACK_BUTTON, INPUT_PULLUP);
  led.begin();
  led.setBrightness(60);
  fill(30, 0, 30);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(200);
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  MDNS.begin("claude-led");

  server.on("/idle",     [] { subagents = 0; setState(IDLE); ok(); });
  server.on("/working",  [] { setState(WORKING); ok(); });
  server.on("/approval", [] { setState(APPROVAL); ok(); });
  server.on("/done",     [] { subagents = 0; setState(DONE); ok(); });
  server.on("/error",    [] { subagents = 0; setState(ERR); ok(); });
  server.on("/compact",  [] { setState(COMPACT); ok(); });

  server.on("/subagent/start", [] { subagents++; setState(WORKING); ok(); });
  server.on("/subagent/stop",  [] { if (subagents > 0) subagents--; ok(); });

  // 鎖存：/latch?id=fail
  server.on("/latch", [] {
    String id = server.arg("id");
    for (int i = 0; i < LATCH_N; i++) {
      if (id == latches[i].id) {
        if (!anyLatched()) cycleAnchor = millis();  // 第一個事件立即開始播放
        if (latches[i].count < 255) latches[i].count++;
      }
    }
    ok();
  });

  // 確認已讀：清除所有鎖存事件
  server.on("/ack", [] { ack(); ok(); });

  // 一次性閃爍：/flash?c=FFFFFF&ms=120&n=3
  server.on("/flash", [] {
    startFlash(parseHex(server.arg("c"), 0xFFFFFF),
               server.hasArg("ms") ? server.arg("ms").toInt() : 150,
               server.hasArg("n")  ? server.arg("n").toInt()  : 1);
    ok();
  });

  server.on("/status", [] {
    String j = String("{\"state\":") + state + ",\"subagents\":" + subagents + ",\"latches\":{";
    for (int i = 0; i < LATCH_N; i++) {
      j += String("\"") + latches[i].id + "\":" + latches[i].count;
      if (i < LATCH_N - 1) j += ",";
    }
    server.send(200, "application/json", j + "}}");
  });

  server.begin();
  setState(IDLE);
}

void loop() {
  server.handleClient();
  checkButton();
  render();
  delay(10);
}
