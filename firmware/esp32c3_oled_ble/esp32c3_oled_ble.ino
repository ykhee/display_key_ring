/*
 * ESP32-C3 + SH1106 1.3" OLED (128x64, I2C) + BLE 스마트폰 제어
 * ------------------------------------------------------------------
 * 기능: 눈 애니메이션(표정 8종, 자동 깜빡임/두리번), 시계, 타이머,
 *       스톱워치, 문자 메시지(한글 지원), 밝기/화면 뒤집기
 *
 * 필요한 것
 *  - Arduino IDE 2.x + "esp32 by Espressif Systems" 보드 패키지 (2.x / 3.x 모두 가능)
 *  - 라이브러리: "U8g2" (by oliver)  ← 라이브러리 매니저에서 설치
 *  - BLE 라이브러리는 ESP32 보드 패키지에 기본 포함 (추가 설치 불필요)
 *
 * 배선 (보드마다 다를 수 있으니 아래 SDA/SCL 핀 번호를 확인하세요)
 *  OLED VCC → 3V3,  GND → GND,  SDA → GPIO 8,  SCL → GPIO 9
 *
 * 블루투스 통신: Nordic UART Service(NUS) UUID 사용, 명령은 텍스트 한 줄
 *  TIME:<epoch초>,<UTC오프셋 분>     시간 동기화 (예: TIME:1763900000,540)
 *  MODE:EYES|CLOCK|TIMER|STOPWATCH|TEXT
 *  EYE:NORMAL|HAPPY|SAD|ANGRY|SLEEPY|SURPRISED|LOVE|WINK
 *  LOOK:<x>,<y>    (-100~100)  눈동자 방향      BLINK   한 번 깜빡
 *  AUTO:1|0        자동 두리번/깜빡임
 *  TIMER:SET,<초> | TIMER:START | TIMER:PAUSE | TIMER:RESET
 *  SW:START | SW:STOP | SW:RESET   스톱워치
 *  TEXT:<문자열>   메시지 표시 (UTF-8, 한글 가능)
 *  BRIGHT:<0~255>  밝기          FLIP:1|0   화면 180도 회전
 *  H24:1|0         24시간제/12시간제
 *  보드 → 앱(notify): "S:<mode>,<타이머남은초>,<타이머동작>,<스톱워치ms>,<스톱워치동작>"
 *                     "EV:TIMER_DONE"
 */

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <U8g2lib.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <time.h>

// ======================= 사용자 설정 =======================
#define I2C_SDA 8        // ESP32-C3 Super Mini / DevKitM 기본 SDA
#define I2C_SCL 9        // ESP32-C3 Super Mini / DevKitM 기본 SCL
#define DEVICE_NAME "ESP32-Eyes"
// 화면이 2픽셀 밀려 보이면 SH1106이 맞는지, 아무것도 안 나오면 I2C 주소(0x3C/0x3D)를 확인
#define OLED_I2C_ADDR 0x3C
// ==========================================================

#define SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // 앱 → 보드 (write)
#define TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // 보드 → 앱 (notify)

// 핀을 생성자에 넘기면 U8g2가 Wire.begin(SDA, SCL)을 대신 호출함
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /*reset=*/U8X8_PIN_NONE, /*clock=*/I2C_SCL, /*data=*/I2C_SDA);

// ---------------------- 상태 변수 ----------------------
enum Mode { M_EYES, M_CLOCK, M_TIMER, M_STOPWATCH, M_TEXT };
enum Mood { NORMAL, HAPPY, SAD, ANGRY, SLEEPY, SURPRISED, LOVE, WINK };
const char* MODE_NAMES[] = {"EYES", "CLOCK", "TIMER", "STOPWATCH", "TEXT"};
const char* MOOD_NAMES[] = {"NORMAL", "HAPPY", "SAD", "ANGRY", "SLEEPY", "SURPRISED", "LOVE", "WINK"};

Mode mode = M_EYES;
Mood mood = NORMAL;

// BLE
BLEServer* server = nullptr;
BLECharacteristic* txChar = nullptr;
bool bleConnected = false;
bool bleWasConnected = false;

// 명령 큐 (BLE 콜백 → loop로 안전하게 전달)
#define CMD_QUEUE_LEN 10
#define CMD_MAX_LEN 200
QueueHandle_t cmdQueue;

// 시간 (보드에 RTC 배터리가 없으므로 앱에서 동기화)
bool timeSynced = false;
uint32_t baseEpoch = 0;       // 동기화 시점의 UTC epoch(초)
uint32_t baseMillis = 0;      // 동기화 시점의 millis()
int32_t tzOffsetSec = 9 * 3600; // 기본 한국 표준시(UTC+9)
bool use24h = true;

// 눈 애니메이션
float lookX = 0, lookY = 0;          // 현재 위치 (-1~1)
float targetX = 0, targetY = 0;      // 목표 위치
bool autoMode = true;
uint32_t nextLookAt = 0, nextBlinkAt = 0, manualUntil = 0;
uint32_t blinkStart = 0;
bool blinking = false;
const uint16_t BLINK_MS = 180;

// 타이머
uint32_t timerSetSec = 300;
uint32_t timerRemainMs = 300000;
bool timerRunning = false;
uint32_t timerLastTick = 0;
bool timerDone = false;
uint32_t timerDoneAt = 0;

// 스톱워치
bool swRunning = false;
uint32_t swAccumMs = 0, swStartMs = 0;

// 텍스트
String message = "안녕하세요!";
uint32_t textPageAt = 0;
int textPage = 0;

uint32_t lastStatusAt = 0;

// ---------------------- BLE 콜백 ----------------------
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override { bleConnected = true; }
  void onDisconnect(BLEServer* s) override { bleConnected = false; }
};

class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    // core 2.x(std::string)와 3.x(String) 모두 호환
    String v = String(c->getValue().c_str());
    v.trim();
    if (v.length() == 0) return;
    char buf[CMD_MAX_LEN];
    strlcpy(buf, v.c_str(), sizeof(buf));
    xQueueSend(cmdQueue, buf, 0);  // 큐가 가득 차면 버림
  }
};

void notify(const String& s) {
  if (!bleConnected || !txChar) return;
  txChar->setValue((uint8_t*)s.c_str(), s.length());
  txChar->notify();
}

// ---------------------- 유틸 ----------------------
uint32_t nowEpoch() {
  return baseEpoch + (millis() - baseMillis) / 1000;
}

uint32_t stopwatchMs() {
  return swAccumMs + (swRunning ? millis() - swStartMs : 0);
}

void scheduleBlink(uint32_t now) { nextBlinkAt = now + random(2000, 6000); }
void scheduleLook(uint32_t now)  { nextLookAt  = now + random(1200, 4000); }

// ---------------------- 명령 처리 ----------------------
void handleCommand(String cmd) {
  int colon = cmd.indexOf(':');
  String key = colon >= 0 ? cmd.substring(0, colon) : cmd;
  String arg = colon >= 0 ? cmd.substring(colon + 1) : "";
  key.toUpperCase();
  uint32_t now = millis();

  if (key == "TIME") {
    int comma = arg.indexOf(',');
    baseEpoch = (uint32_t)strtoul(arg.substring(0, comma < 0 ? arg.length() : comma).c_str(), nullptr, 10);
    if (comma >= 0) tzOffsetSec = arg.substring(comma + 1).toInt() * 60;
    baseMillis = now;
    timeSynced = true;
  } else if (key == "MODE") {
    arg.toUpperCase();
    for (int i = 0; i < 5; i++) if (arg == MODE_NAMES[i]) mode = (Mode)i;
    if (mode == M_TEXT) { textPage = 0; textPageAt = now; }
  } else if (key == "EYE") {
    arg.toUpperCase();
    for (int i = 0; i < 8; i++) if (arg == MOOD_NAMES[i]) mood = (Mood)i;
    mode = M_EYES;
  } else if (key == "LOOK") {
    int comma = arg.indexOf(',');
    if (comma > 0) {
      targetX = constrain(arg.substring(0, comma).toInt(), -100, 100) / 100.0f;
      targetY = constrain(arg.substring(comma + 1).toInt(), -100, 100) / 100.0f;
      manualUntil = now + 5000;   // 5초간 자동 두리번 중지
    }
    mode = M_EYES;
  } else if (key == "BLINK") {
    blinking = true; blinkStart = now; mode = M_EYES;
  } else if (key == "AUTO") {
    autoMode = arg.toInt() != 0;
  } else if (key == "TIMER") {
    arg.toUpperCase();
    if (arg.startsWith("SET")) {
      int comma = arg.indexOf(',');
      long s = comma > 0 ? arg.substring(comma + 1).toInt() : 0;
      timerSetSec = constrain(s, 1, 99L * 60 + 59);
      timerRemainMs = timerSetSec * 1000UL;
      timerRunning = false; timerDone = false;
    } else if (arg == "START") {
      if (timerRemainMs == 0) timerRemainMs = timerSetSec * 1000UL;
      timerRunning = true; timerDone = false; timerLastTick = now;
    } else if (arg == "PAUSE") {
      timerRunning = false;
    } else if (arg == "RESET") {
      timerRunning = false; timerDone = false;
      timerRemainMs = timerSetSec * 1000UL;
    }
    mode = M_TIMER;
  } else if (key == "SW") {
    arg.toUpperCase();
    if (arg == "START" && !swRunning) { swRunning = true; swStartMs = now; }
    else if (arg == "STOP" && swRunning) { swAccumMs += now - swStartMs; swRunning = false; }
    else if (arg == "RESET") { swRunning = false; swAccumMs = 0; }
    mode = M_STOPWATCH;
  } else if (key == "TEXT") {
    message = arg;
    textPage = 0; textPageAt = now;
    mode = M_TEXT;
  } else if (key == "BRIGHT") {
    u8g2.setContrast((uint8_t)constrain(arg.toInt(), 0, 255));
  } else if (key == "FLIP") {
    u8g2.setDisplayRotation(arg.toInt() ? U8G2_R2 : U8G2_R0);
  } else if (key == "H24") {
    use24h = arg.toInt() != 0;
  } else if (key == "PING") {
    // 상태만 회신
  }
  lastStatusAt = 0;  // 다음 loop에서 바로 상태 전송
}

// ---------------------- 눈 그리기 ----------------------
void drawHeart(int cx, int cy, int s) {
  int r = s / 4;
  u8g2.drawDisc(cx - r, cy - r / 2, r);
  u8g2.drawDisc(cx + r, cy - r / 2, r);
  u8g2.drawTriangle(cx - 2 * r - 1, cy - r / 4, cx + 2 * r + 1, cy - r / 4, cx, cy + 2 * r);
}

// side: -1 = 왼쪽 눈, +1 = 오른쪽 눈 (화면 기준)
void drawEye(int cx, int cy, int side, float openness) {
  const int W = 34, H = 38, R = 9;
  int w = W, h = H;

  if (mood == SURPRISED) { w = 40; h = 42; }

  if (mood == LOVE) {
    if (openness < 0.3f) { u8g2.drawBox(cx - w / 2, cy - 1, w, 3); return; }
    drawHeart(cx, cy, 40);
    return;
  }

  // 윙크: 오른쪽 눈은 웃는 곡선
  if (mood == WINK && side > 0) {
    for (int t = 0; t < 3; t++) {
      u8g2.drawCircle(cx, cy + 10 + t, 14, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
    }
    return;
  }

  // 깜빡임 적용
  h = max(2, (int)(h * openness));
  int x = cx - w / 2, y = cy - h / 2;

  if (mood == SURPRISED) {
    int r = min(w, h) / 2;
    if (h < 8) u8g2.drawBox(x, y, w, h);
    else {
      u8g2.drawDisc(cx, cy, r);
      u8g2.setDrawColor(0);
      u8g2.drawDisc(cx, cy, r - 5);   // 동그란 링 눈
      u8g2.setDrawColor(1);
      u8g2.drawDisc(cx, cy, 6);       // 눈동자
    }
    return;
  }

  int r = min(R, h / 2 - 1);
  if (r < 1) u8g2.drawBox(x, y, w, h);
  else u8g2.drawRBox(x, y, w, h, r);

  if (h < 10) return;  // 거의 감은 상태면 표정 덮개 생략

  // 표정별로 검정색으로 일부를 지워서 모양 만들기
  u8g2.setDrawColor(0);
  int inner = (side < 0) ? x + w : x;   // 얼굴 중앙 쪽 가장자리
  int outer = (side < 0) ? x : x + w;   // 바깥쪽 가장자리
  switch (mood) {
    case HAPPY:   // 아래쪽을 둥글게 파내서 ^ ^ 모양
      u8g2.drawFilledEllipse(cx, y + h + 4, w / 2 + 4, h / 2 + 2);
      break;
    case SAD:     // 바깥쪽이 처진 눈꺼풀
      u8g2.drawTriangle(inner, y - 1, outer, y - 1, outer, y + h / 2);
      break;
    case ANGRY:   // 안쪽이 내려간 눈꺼풀
      u8g2.drawTriangle(outer, y - 1, inner, y - 1, inner, y + h / 2);
      break;
    case SLEEPY:  // 위쪽 절반을 덮은 졸린 눈
      u8g2.drawBox(x - 1, y - 1, w + 2, h / 2 + 1);
      break;
    default:
      break;
  }
  u8g2.setDrawColor(1);
}

void updateEyes(uint32_t now) {
  // 자동 두리번
  if (autoMode && now > manualUntil && now > nextLookAt) {
    if (random(3) == 0) { targetX = 0; targetY = 0; }
    else { targetX = random(-100, 101) / 100.0f; targetY = random(-60, 61) / 100.0f; }
    scheduleLook(now);
  }
  // 자동 깜빡임
  if (autoMode && !blinking && now > nextBlinkAt) {
    blinking = true; blinkStart = now; scheduleBlink(now);
  }
  // 부드럽게 이동
  lookX += (targetX - lookX) * 0.25f;
  lookY += (targetY - lookY) * 0.25f;
}

void drawEyesScreen(uint32_t now) {
  float open = 1.0f;
  if (blinking) {
    uint32_t t = now - blinkStart;
    if (t >= BLINK_MS) blinking = false;
    else open = fabsf((float)t / BLINK_MS * 2.0f - 1.0f);  // 1 → 0 → 1
  }
  if (mood == SLEEPY) open *= 0.85f;

  int dx = (int)(lookX * 16);
  int dy = (int)(lookY * 9);
  drawEye(36 + dx, 32 + dy, -1, open);
  drawEye(92 + dx, 32 + dy, +1, open);
  if (mood == SLEEPY && (now / 700) % 3 != 0) {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(112, 10, "z");
    if ((now / 700) % 3 == 2) u8g2.drawStr(118, 6, "Z");
  }
}

// ---------------------- 시계 ----------------------
void drawClockScreen() {
  if (!timeSynced) {
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(10, 28, "Time not synced");
    u8g2.drawStr(10, 44, "Connect the app");
    return;
  }
  time_t t = (time_t)nowEpoch() + tzOffsetSec;
  struct tm tmv;
  gmtime_r(&t, &tmv);

  int hour = tmv.tm_hour;
  const char* ampm = "";
  if (!use24h) { ampm = hour < 12 ? "AM" : "PM"; hour = hour % 12; if (hour == 0) hour = 12; }

  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d", hour, tmv.tm_min);
  u8g2.setFont(u8g2_font_logisoso32_tn);
  int w = u8g2.getStrWidth(buf);
  u8g2.drawStr((128 - w) / 2 - 6, 44, buf);

  u8g2.setFont(u8g2_font_6x10_tf);
  snprintf(buf, sizeof(buf), "%02d", tmv.tm_sec);
  u8g2.drawStr(116, 44, buf);
  if (!use24h) u8g2.drawStr(116, 20, ampm);

  static const char* WD[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  char date[24];
  snprintf(date, sizeof(date), "%04d-%02d-%02d %s", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, WD[tmv.tm_wday]);
  w = u8g2.getStrWidth(date);
  u8g2.drawStr((128 - w) / 2, 60, date);
}

// ---------------------- 타이머 ----------------------
void updateTimer(uint32_t now) {
  if (!timerRunning) return;
  uint32_t dt = now - timerLastTick;
  timerLastTick = now;
  if (dt >= timerRemainMs) {
    timerRemainMs = 0;
    timerRunning = false;
    timerDone = true;
    timerDoneAt = now;
    mode = M_TIMER;
    notify("EV:TIMER_DONE");
  } else {
    timerRemainMs -= dt;
  }
}

void drawTimerScreen(uint32_t now) {
  if (timerDone && now - timerDoneAt < 15000) {
    bool on = (now / 300) % 2;   // 깜빡이는 알림
    if (on) u8g2.drawBox(0, 0, 128, 64);
    u8g2.setDrawColor(on ? 0 : 1);
    u8g2.setFont(u8g2_font_logisoso22_tr);
    int w = u8g2.getStrWidth("TIME UP!");
    u8g2.drawStr((128 - w) / 2, 44, "TIME UP!");
    u8g2.setDrawColor(1);
    return;
  }
  uint32_t sec = (timerRemainMs + 999) / 1000;  // 올림 표시
  char buf[8];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "TIMER");
  u8g2.drawStr(128 - u8g2.getStrWidth(timerRunning ? "RUN" : "PAUSE"), 9, timerRunning ? "RUN" : "PAUSE");

  u8g2.setFont(u8g2_font_logisoso32_tn);
  int w = u8g2.getStrWidth(buf);
  u8g2.drawStr((128 - w) / 2, 48, buf);

  // 진행 막대
  u8g2.drawFrame(4, 55, 120, 8);
  uint32_t total = timerSetSec * 1000UL;
  int fill = total ? (int)(116.0f * (float)timerRemainMs / total) : 0;
  if (fill > 0) u8g2.drawBox(6, 57, fill, 4);
}

// ---------------------- 스톱워치 ----------------------
void drawStopwatchScreen() {
  uint32_t ms = stopwatchMs();
  uint32_t m = ms / 60000, s = (ms / 1000) % 60, cs = (ms / 10) % 100;
  char buf[12];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(m % 100), (unsigned long)s);

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 9, "STOPWATCH");
  u8g2.setFont(u8g2_font_logisoso32_tn);
  u8g2.drawStr(4, 50, buf);
  u8g2.setFont(u8g2_font_logisoso16_tn);
  snprintf(buf, sizeof(buf), "%02lu", (unsigned long)cs);
  u8g2.drawStr(104, 50, buf);
  if (swRunning && (millis() / 500) % 2) u8g2.drawDisc(122, 6, 3);
}

// ---------------------- 텍스트 (한글 자동 줄바꿈) ----------------------
// UTF-8 문자열을 화면 폭에 맞게 줄 단위로 나눔
int wrapText(const String& s, String* lines, int maxLines, int maxWidth) {
  int count = 0;
  String cur = "";
  int i = 0, n = s.length();
  while (i < n && count < maxLines) {
    uint8_t c = s[i];
    int len = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    String ch = s.substring(i, i + len);
    i += len;
    if (ch == "\n" || (ch == "|")) {  // '|' 로 강제 줄바꿈 가능
      lines[count++] = cur; cur = ""; continue;
    }
    String test = cur + ch;
    if (u8g2.getUTF8Width(test.c_str()) > maxWidth) {
      lines[count++] = cur;
      cur = (ch == " ") ? "" : ch;
    } else {
      cur = test;
    }
  }
  if (count < maxLines && cur.length()) lines[count++] = cur;
  return count;
}

void drawTextScreen(uint32_t now) {
  // unifont 한글 폰트: 글자 높이 16px → 한 화면 4줄, 한 줄에 한글 8자 정도
  u8g2.setFont(u8g2_font_unifont_t_korean2);
  static String lines[16];
  int n = wrapText(message, lines, 16, 128);
  int pages = (n + 3) / 4;
  if (pages > 1 && now - textPageAt > 2500) { textPage = (textPage + 1) % pages; textPageAt = now; }
  if (textPage >= pages) textPage = 0;

  int start = textPage * 4;
  int shown = min(4, n - start);
  int top = (64 - shown * 16) / 2;  // 세로 가운데 정렬
  for (int k = 0; k < shown; k++) {
    const char* ln = lines[start + k].c_str();
    int w = u8g2.getUTF8Width(ln);
    u8g2.drawUTF8(max(0, (128 - w) / 2), top + 14 + k * 16, ln);
  }
}

// ---------------------- setup / loop ----------------------
void setupBLE() {
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(185);  // 긴 메시지 전송용
  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCB());

  BLEService* svc = server->createService(SERVICE_UUID);
  txChar = svc->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  txChar->addDescriptor(new BLE2902());
  BLECharacteristic* rx = svc->createCharacteristic(
      RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCB());
  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);   // 이름은 스캔 응답에 실림
  BLEDevice::startAdvertising();
}

void setup() {
  Serial.begin(115200);
  cmdQueue = xQueueCreate(CMD_QUEUE_LEN, CMD_MAX_LEN);
  u8g2.setBusClock(400000);
  u8g2.setI2CAddress(OLED_I2C_ADDR * 2);  // U8g2는 8비트 주소 사용
  u8g2.begin();
  u8g2.enableUTF8Print();
  u8g2.setContrast(200);

  randomSeed(esp_random());
  uint32_t now = millis();
  scheduleBlink(now);
  scheduleLook(now);

  // 부팅 화면
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(22, 30, DEVICE_NAME);
  u8g2.drawStr(16, 46, "Waiting for BLE...");
  u8g2.sendBuffer();

  setupBLE();
  Serial.println("BLE advertising as " DEVICE_NAME);
  delay(800);
}

void loop() {
  uint32_t now = millis();

  // 큐에 쌓인 명령 처리
  char cbuf[CMD_MAX_LEN];
  while (xQueueReceive(cmdQueue, cbuf, 0) == pdTRUE) {
    String cmd(cbuf);
    Serial.println("CMD " + cmd);
    handleCommand(cmd);
  }

  // 연결 끊기면 다시 광고 시작
  if (!bleConnected && bleWasConnected) {
    delay(300);
    server->startAdvertising();
    bleWasConnected = false;
  }
  if (bleConnected && !bleWasConnected) bleWasConnected = true;

  updateTimer(now);
  updateEyes(now);

  // 화면 그리기
  u8g2.clearBuffer();
  switch (mode) {
    case M_EYES:      drawEyesScreen(now); break;
    case M_CLOCK:     drawClockScreen(); break;
    case M_TIMER:     drawTimerScreen(now); break;
    case M_STOPWATCH: drawStopwatchScreen(); break;
    case M_TEXT:      drawTextScreen(now); break;
  }
  if (!bleConnected && mode != M_EYES) {   // 연결 안 됨 표시(작은 점선)
    for (int x = 0; x < 6; x += 2) u8g2.drawPixel(x, 63);
  }
  u8g2.sendBuffer();

  // 1초마다 상태 전송
  if (bleConnected && now - lastStatusAt > 1000) {
    lastStatusAt = now;
    char st[64];
    snprintf(st, sizeof(st), "S:%s,%lu,%d,%lu,%d", MODE_NAMES[mode],
             (unsigned long)((timerRemainMs + 999) / 1000), timerRunning ? 1 : 0,
             (unsigned long)stopwatchMs(), swRunning ? 1 : 0);
    notify(st);
  }

  delay(15);  // 약 50fps
}
