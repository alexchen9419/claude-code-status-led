// Claude Code 狀態燈 - COM port 版（ESP32-S3 + 板載 WS2812）
// 由電腦上的 led_bridge.py 透過序列埠送指令，一行一個指令：
//   idle | working | approval | done | error | compact
//   sub+ | sub- | latch <id> | ack | flash <RRGGBB> <ms> <n> | ping | status
// 需要函式庫：Adafruit NeoPixel
// Arduino IDE 設定：用原生 USB 口時，Tools → USB CDC On Boot → Enabled
#include <Adafruit_NeoPixel.h>

#define LED_PIN    48   // DevKitC-1 v1.0 = 48，v1.1 = 38
#define LED_COUNT  1
#define ACK_BUTTON 0    // BOOT 鍵
#define LINK_TIMEOUT 15000  // 超過 15 秒沒收到任何指令 = bridge 斷線

Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// ================= 基礎狀態 =================
enum State { IDLE, WORKING, APPROVAL, DONE, ERR, COMPACT };
State state = IDLE;
int subagents = 0;
unsigned long lastRx = 0;
bool everConnected = false;

// ================= 鎖存事件 =================
struct Latch { const char* id; uint32_t color; uint8_t count; };
Latch latches[] = {
  { "fail",   0xFF4000, 0 },
  { "denied", 0xFF0000, 0 },
  { "task",   0x00FF00, 0 },
};
const int LATCH_N = sizeof(latches) / sizeof(latches[0]);
unsigned long cycleAnchor = 0;

const uint16_t BASE_MS  = 2500;
const uint16_t BLINK_MS = 150;
const uint16_t GAP_MS   = 500;
const uint8_t  MAX_SHOW = 5;

bool anyLatched() {
  for (int i = 0; i < LATCH_N; i++) if (latches[i].count) return true;
  return false;
}
void ack() { for (int i = 0; i < LATCH_N; i++) latches[i].count = 0; }

// ================= 一次性閃爍 =================
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
  // bridge 斷線：暗紫色慢閃
  if (everConnected && now - lastRx > LINK_TIMEOUT) {
    if ((now / 1000) % 2) fill(25, 0, 25); else fill(0, 0, 0);
    return;
  }
  switch (state) {
    case IDLE: fill(3, 3, 3); break;
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
    case APPROVAL:
      if ((now / 300) % 2) fill(255, 120, 0); else fill(0, 0, 0);
      break;
    case DONE: {
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

bool renderLatches(unsigned long now) {
  if (!anyLatched()) return false;
  unsigned long overlay = 0;
  for (int i = 0; i < LATCH_N; i++)
    if (latches[i].count)
      overlay += (unsigned long)min(latches[i].count, MAX_SHOW) * BLINK_MS * 2 + GAP_MS;

  unsigned long pos = (now - cycleAnchor) % (overlay + BASE_MS);
  if (pos >= overlay) return false;

  for (int i = 0; i < LATCH_N; i++) {
    if (!latches[i].count) continue;
    unsigned long seg = (unsigned long)min(latches[i].count, MAX_SHOW) * BLINK_MS * 2;
    if (pos < seg) { fillRGB((pos % (BLINK_MS * 2)) < BLINK_MS ? latches[i].color : 0); return true; }
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
    if (cur == LOW) {
      ack();
      if (state == DONE || state == ERR) state = IDLE;
      startFlash(0xFFFFFF, 80, 1);
      Serial.println("{\"event\":\"button_ack\"}");
    }
  }
}

// ================= 序列埠指令 =================
void sendStatus() {
  String j = String("{\"state\":") + state + ",\"subagents\":" + subagents + ",\"latches\":{";
  for (int i = 0; i < LATCH_N; i++) {
    j += String("\"") + latches[i].id + "\":" + latches[i].count;
    if (i < LATCH_N - 1) j += ",";
  }
  Serial.println(j + "}}");
}

void handleCmd(String line) {
  line.trim();
  if (!line.length()) return;
  lastRx = millis();
  everConnected = true;

  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? ""   : line.substring(sp + 1);

  if      (cmd == "idle")     { subagents = 0; state = IDLE; }
  else if (cmd == "working")  { state = WORKING; }
  else if (cmd == "approval") { state = APPROVAL; }
  else if (cmd == "done")     { subagents = 0; state = DONE; }
  else if (cmd == "error")    { subagents = 0; state = ERR; }
  else if (cmd == "compact")  { state = COMPACT; }
  else if (cmd == "sub+")     { subagents++; state = WORKING; }
  else if (cmd == "sub-")     { if (subagents > 0) subagents--; }
  else if (cmd == "ack")      { ack(); }
  else if (cmd == "ping")     { /* 只更新 lastRx */ }
  else if (cmd == "status")   { sendStatus(); return; }
  else if (cmd == "latch") {
    for (int i = 0; i < LATCH_N; i++) {
      if (arg == latches[i].id) {
        if (!anyLatched()) cycleAnchor = millis();
        if (latches[i].count < 255) latches[i].count++;
      }
    }
  }
  else if (cmd == "flash") {
    char hex[7] = {0}; int ms = 150, n = 1;
    sscanf(arg.c_str(), "%6s %d %d", hex, &ms, &n);
    startFlash(strtoul(hex, nullptr, 16), ms, n);
  }
  else { Serial.println("{\"err\":\"unknown\"}"); return; }

  Serial.println("ok");
}

void pollSerial() {
  static String buf;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { handleCmd(buf); buf = ""; }
    else if (buf.length() < 64) buf += c;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(ACK_BUTTON, INPUT_PULLUP);
  led.begin();
  led.setBrightness(60);
  fill(3, 3, 3);
}

void loop() {
  pollSerial();
  checkButton();
  render();
  delay(5);
}
