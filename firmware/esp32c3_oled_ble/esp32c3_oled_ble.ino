/*
 * ESP32-C3 + SH1106 1.3" OLED (128x64, I2C) 로봇 얼굴 + BLE 스마트폰 제어
 * -----------------------------------------------------------------------
 * 화면: 로봇 얼굴(눈 4종 × 표정 10종, 입·볼터치), 시계, 디데이(3개),
 *       타이머, 스톱워치, 메시지(한글), 자동 순환
 * 설정은 보드에 저장되어 전원을 껐다 켜도 유지됩니다. (시간은 제외)
 *
 * 필요한 것
 *  - Arduino IDE 2.x + "esp32 by Espressif Systems" 보드 패키지
 *  - 라이브러리 "U8g2" (by oliver)
 *  - 같은 폴더의 face.h, screens.h  (스케치를 열면 탭으로 함께 열림)
 *  - Tools → Partition Scheme → "Huge APP (3MB No OTA/1MB SPIFFS)"
 *    (한글 폰트 + 블루투스로 용량이 커서 기본 설정이면 "Sketch too big" 오류가 날 수 있음)
 *
 * 배선: OLED VCC→3V3, GND→GND, SDA→GPIO8, SCL→GPIO9
 *
 * 통신(Nordic UART Service UUID). 명령은 한 줄 텍스트, 끝에 '\n'
 *  TIME:<epoch>,<UTC오프셋분>  MODE:EYES|CLOCK|DDAY|TIMER|STOPWATCH|TEXT|CYCLE
 *  EYE:<표정>  STYLE:<0~3>  MOUTH:0|1  BLUSH:0|1  LOOK:x,y  BLINK  AUTO:0|1
 *  DDAY:<0~2>,<YYYY-MM-DD>,<제목>   DDAY:DEL,<0~2>
 *  TIMER:SET,<초>|START|PAUSE|RESET   SW:START|STOP|RESET   TEXT:<문자열>
 *  CYCLE:<초>,<모드비트마스크>   BRIGHT:<0~255>  FLIP:0|1  H24:0|1  GET
 */

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <time.h>
#include <strings.h>
#include "face.h"
#include "screens.h"

// ======================= 사용자 설정 =======================
#define I2C_SDA 8
#define I2C_SCL 9
#define OLED_I2C_ADDR 0x3C          // 화면이 안 나오면 0x3D 로
#define DEVICE_NAME "ESP32-Eyes"
// ==========================================================

#define SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   // 폰 → 보드
#define TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   // 보드 → 폰

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /*reset=*/U8X8_PIN_NONE, /*clock=*/I2C_SCL, /*data=*/I2C_SDA);
Preferences prefs;

// ---------------------- 화면 모드 ----------------------
enum Mode : uint8_t { M_EYES, M_CLOCK, M_DDAY, M_TIMER, M_STOPWATCH, M_TEXT, M_COUNT };
const char* const MODE_NAMES[M_COUNT] = {"EYES", "CLOCK", "DDAY", "TIMER", "STOPWATCH", "TEXT"};

Mode mode = M_EYES;
Face face;

// 자동 순환
bool cycleOn = false;
uint16_t cycleSec = 10;
uint8_t cycleMask = (1 << M_EYES) | (1 << M_CLOCK) | (1 << M_DDAY);
uint32_t cycleAt = 0;

// 화면 설정
uint8_t brightness = 200;
bool flipped = false;
bool use24h = true;

// 시간 (RTC 배터리가 없어 앱에서 동기화)
bool timeSynced = false;
uint32_t baseEpoch = 0, baseMillis = 0;
int32_t tzOffsetSec = 9 * 3600;

// 디데이
struct Dday { bool used; int16_t y; int8_t m, d; char title[40]; };
Dday ddays[3];
int ddayPos = 0;          // 저장된 디데이 중 몇 번째를 보여주는지
uint32_t ddayAt = 0;

// 타이머 / 스톱워치
uint32_t timerSetSec = 300, timerRemainMs = 300000, timerLastTick = 0, timerDoneAt = 0;
bool timerRunning = false, timerDone = false;
bool swRunning = false;
uint32_t swAccumMs = 0, swStartMs = 0;

// 메시지
char message[160] = "안녕하세요!";
int textPage = 0;
uint32_t textPageAt = 0;

// 저장 예약 (플래시 수명 보호: 변경 후 3초 뒤 한 번에 저장)
bool saveDirty = false;
uint32_t saveAt = 0;
void markDirty() { saveDirty = true; saveAt = millis() + 3000; }

// ---------------------- BLE ----------------------
BLEServer* server = nullptr;
BLECharacteristic* txChar = nullptr;
volatile bool bleConnected = false;
bool bleWasConnected = false;
bool sendConfigPending = false;

#define CMD_QUEUE_LEN 12
#define CMD_MAX_LEN 200
QueueHandle_t cmdQueue;
char rxBuf[CMD_MAX_LEN];
int rxLen = 0;

class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { bleConnected = true; }
  void onDisconnect(BLEServer*) override { bleConnected = false; }
};

// 앱은 긴 명령을 20바이트씩 나눠 보내고 마지막에 '\n'을 붙임
class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String v = String(c->getValue().c_str());   // core 2.x / 3.x 호환
    bool sawNewline = false;
    for (size_t i = 0; i < v.length(); i++) {
      char ch = v[i];
      if (ch == '\n' || ch == '\r') {
        sawNewline = true;
        if (rxLen > 0) { rxBuf[rxLen] = 0; xQueueSend(cmdQueue, rxBuf, 0); rxLen = 0; }
      } else if (rxLen < CMD_MAX_LEN - 1) {
        rxBuf[rxLen++] = ch;
      }
    }
    // nRF Connect 등에서 '\n' 없이 짧게 보낸 명령도 처리
    if (!sawNewline && v.length() < 20 && rxLen > 0) {
      rxBuf[rxLen] = 0; xQueueSend(cmdQueue, rxBuf, 0); rxLen = 0;
    }
  }
};

// 한 줄을 MTU에 맞춰 나눠 보냄 (끝에 '\n')
void sendLine(const char* s) {
  if (!bleConnected || !txChar) return;
  uint16_t mtu = server->getPeerMTU(server->getConnId());
  size_t chunk = (mtu > 23) ? mtu - 3 : 20;
  if (chunk > 180) chunk = 180;
  size_t n = strlen(s), total = n + 1, off = 0;
  uint8_t buf[180];
  while (off < total) {
    size_t len = total - off < chunk ? total - off : chunk;
    for (size_t k = 0; k < len; k++) buf[k] = (off + k < n) ? (uint8_t)s[off + k] : '\n';
    txChar->setValue(buf, len);
    txChar->notify();
    off += len;
    if (off < total) delay(8);
  }
}

// ---------------------- 시간 ----------------------
uint32_t nowEpoch() { return baseEpoch + (millis() - baseMillis) / 1000; }

bool localTime(struct tm& out) {
  if (!timeSynced) return false;
  time_t t = (time_t)nowEpoch() + tzOffsetSec;
  gmtime_r(&t, &out);
  return true;
}

int32_t todayDays() { return (int32_t)(((int64_t)nowEpoch() + tzOffsetSec) / 86400); }

uint32_t stopwatchMs() { return swAccumMs + (swRunning ? millis() - swStartMs : 0); }

// ---------------------- 설정 저장/불러오기 ----------------------
void saveSettings() {
  prefs.putUChar("style", face.style);
  prefs.putUChar("mood", face.mood);
  prefs.putBool("mouth", face.mouth);
  prefs.putBool("blush", face.blush);
  prefs.putBool("auto", face.autoLook);
  prefs.putUChar("bright", brightness);
  prefs.putBool("flip", flipped);
  prefs.putBool("h24", use24h);
  prefs.putBool("cyc", cycleOn);
  prefs.putUShort("cycSec", cycleSec);
  prefs.putUChar("cycMask", cycleMask);
  prefs.putUChar("mode", (mode == M_TIMER || mode == M_STOPWATCH) ? M_EYES : mode);
  prefs.putString("msg", message);
  for (int i = 0; i < 3; i++) {
    char key[4] = {'d', 'd', (char)('0' + i), 0};
    char val[64] = "";
    if (ddays[i].used) snprintf(val, sizeof(val), "%04d-%02d-%02d,%s", ddays[i].y, ddays[i].m, ddays[i].d, ddays[i].title);
    prefs.putString(key, val);
  }
  saveDirty = false;
}

bool parseDday(const char* s, Dday& d) {   // "YYYY-MM-DD,제목"
  int y, m, dd, n = 0;
  if (sscanf(s, "%d-%d-%d%n", &y, &m, &dd, &n) != 3) return false;
  if (y < 1970 || y > 2200 || m < 1 || m > 12 || dd < 1 || dd > 31) return false;
  d.used = true; d.y = y; d.m = m; d.d = dd;
  const char* t = s + n;
  if (*t == ',') t++;
  strlcpy(d.title, t, sizeof(d.title));
  return true;
}

void loadSettings() {
  face.style = (EyeStyle)constrain(prefs.getUChar("style", STYLE_SQUARE), 0, STYLE_COUNT - 1);
  face.mood = (Mood)constrain(prefs.getUChar("mood", MOOD_NORMAL), 0, MOOD_COUNT - 1);
  face.mouth = prefs.getBool("mouth", true);
  face.blush = prefs.getBool("blush", true);
  face.autoLook = prefs.getBool("auto", true);
  brightness = prefs.getUChar("bright", 200);
  flipped = prefs.getBool("flip", false);
  use24h = prefs.getBool("h24", true);
  cycleOn = prefs.getBool("cyc", false);
  cycleSec = constrain(prefs.getUShort("cycSec", 10), 3, 3600);
  cycleMask = prefs.getUChar("cycMask", cycleMask);
  mode = (Mode)constrain(prefs.getUChar("mode", M_EYES), 0, M_COUNT - 1);
  String m = prefs.getString("msg", message);
  strlcpy(message, m.c_str(), sizeof(message));
  for (int i = 0; i < 3; i++) {
    char key[4] = {'d', 'd', (char)('0' + i), 0};
    String v = prefs.getString(key, "");
    ddays[i].used = false;
    if (v.length()) parseDday(v.c_str(), ddays[i]);
  }
}

void sendConfig() {
  char buf[200];
  snprintf(buf, sizeof(buf), "CFG:%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
           face.style, face.mood, face.mouth, face.blush, face.autoLook,
           brightness, flipped, use24h, cycleOn, cycleSec, cycleMask);
  sendLine(buf);
  for (int i = 0; i < 3; i++) {
    if (ddays[i].used) snprintf(buf, sizeof(buf), "DD:%d,%04d-%02d-%02d,%s", i, ddays[i].y, ddays[i].m, ddays[i].d, ddays[i].title);
    else snprintf(buf, sizeof(buf), "DD:%d,", i);
    sendLine(buf);
  }
  snprintf(buf, sizeof(buf), "MSG:%s", message);
  sendLine(buf);
}

void sendStatus() {
  char st[80];
  snprintf(st, sizeof(st), "S:%s,%lu,%d,%lu,%d,%d,%d", MODE_NAMES[mode],
           (unsigned long)((timerRemainMs + 999) / 1000), timerRunning,
           (unsigned long)stopwatchMs(), swRunning, cycleOn, timeSynced);
  sendLine(st);
}

// ---------------------- 화면 모드 전환 ----------------------
void setMode(Mode m) {
  mode = m;
  if (m == M_TEXT) { textPage = 0; textPageAt = millis(); }
  if (m == M_DDAY) { ddayAt = millis(); }
}

void nextCycleMode() {
  for (int k = 1; k <= M_COUNT; k++) {
    Mode m = (Mode)((mode + k) % M_COUNT);
    if (cycleMask & (1 << m)) { setMode(m); return; }
  }
}

// ---------------------- 명령 처리 ----------------------
void handleCommand(char* cmd) {
  char* arg = strchr(cmd, ':');
  if (arg) *arg++ = 0; else arg = (char*)"";
  String key(cmd); key.toUpperCase();
  uint32_t now = millis();

  if (key == "TIME") {
    baseEpoch = strtoul(arg, nullptr, 10);
    char* c = strchr(arg, ',');
    if (c) tzOffsetSec = atoi(c + 1) * 60;
    baseMillis = now; timeSynced = true;
  } else if (key == "GET") {
    sendConfigPending = true;
  } else if (key == "MODE") {
    String a(arg); a.toUpperCase();
    if (a == "CYCLE") { cycleOn = true; cycleAt = now; nextCycleMode(); }
    else for (int i = 0; i < M_COUNT; i++) if (a == MODE_NAMES[i]) { cycleOn = false; setMode((Mode)i); }
    markDirty();
  } else if (key == "EYE") {
    String a(arg); a.toUpperCase();
    for (int i = 0; i < MOOD_COUNT; i++) if (a == MOOD_NAMES[i]) face.mood = (Mood)i;
    cycleOn = false; setMode(M_EYES); markDirty();
  } else if (key == "STYLE") {
    face.style = (EyeStyle)constrain(atoi(arg), 0, STYLE_COUNT - 1);
    cycleOn = false; setMode(M_EYES); markDirty();
  } else if (key == "MOUTH") {
    face.mouth = atoi(arg); setMode(M_EYES); markDirty();
  } else if (key == "BLUSH") {
    face.blush = atoi(arg); setMode(M_EYES); markDirty();
  } else if (key == "LOOK") {
    int x = 0, y = 0;
    if (sscanf(arg, "%d,%d", &x, &y) == 2) {
      faceLook(face, constrain(x, -100, 100) / 100.0f, constrain(y, -100, 100) / 100.0f, now, 5000);
    }
    if (mode != M_EYES) { cycleOn = false; setMode(M_EYES); }
  } else if (key == "BLINK") {
    faceBlink(face, now);
    if (mode != M_EYES) { cycleOn = false; setMode(M_EYES); }
  } else if (key == "AUTO") {
    face.autoLook = atoi(arg); markDirty();
  } else if (key == "DDAY") {
    if (strncasecmp(arg, "DEL,", 4) == 0) {
      int i = atoi(arg + 4);
      if (i >= 0 && i < 3) ddays[i].used = false;
    } else {
      int i = atoi(arg);
      char* c = strchr(arg, ',');
      if (i >= 0 && i < 3 && c) {
        Dday d;
        if (parseDday(c + 1, d)) {
          ddays[i] = d;
          ddayPos = 0;
          for (int k = 0; k < i; k++) if (ddays[k].used) ddayPos++;
          cycleOn = false; setMode(M_DDAY);
        }
      }
    }
    markDirty();
  } else if (key == "TIMER") {
    String a(arg); a.toUpperCase();
    if (a.startsWith("SET")) {
      char* c = strchr(arg, ',');
      long s = c ? atol(c + 1) : 0;
      timerSetSec = constrain(s, 1L, 99L * 60 + 59);
      timerRemainMs = timerSetSec * 1000UL;
      timerRunning = false; timerDone = false;
    } else if (a == "START") {
      if (timerRemainMs == 0) timerRemainMs = timerSetSec * 1000UL;
      timerRunning = true; timerDone = false; timerLastTick = now;
    } else if (a == "PAUSE") {
      timerRunning = false;
    } else if (a == "RESET") {
      timerRunning = false; timerDone = false; timerRemainMs = timerSetSec * 1000UL;
    }
    cycleOn = false; setMode(M_TIMER);
  } else if (key == "SW") {
    String a(arg); a.toUpperCase();
    if (a == "START" && !swRunning) { swRunning = true; swStartMs = now; }
    else if (a == "STOP" && swRunning) { swAccumMs += now - swStartMs; swRunning = false; }
    else if (a == "RESET") { swRunning = false; swAccumMs = 0; }
    cycleOn = false; setMode(M_STOPWATCH);
  } else if (key == "TEXT") {
    strlcpy(message, arg, sizeof(message));
    cycleOn = false; setMode(M_TEXT); markDirty();
  } else if (key == "CYCLE") {
    int sec = 10, mask = cycleMask;
    if (sscanf(arg, "%d,%d", &sec, &mask) >= 1) {
      cycleSec = constrain(sec, 3, 3600);
      if (mask & 0x3F) cycleMask = mask & 0x3F;
      cycleAt = now;
    }
    markDirty();
  } else if (key == "BRIGHT") {
    brightness = constrain(atoi(arg), 0, 255);
    u8g2.setContrast(brightness); markDirty();
  } else if (key == "FLIP") {
    flipped = atoi(arg);
    u8g2.setDisplayRotation(flipped ? U8G2_R2 : U8G2_R0); markDirty();
  } else if (key == "H24") {
    use24h = atoi(arg); markDirty();
  }
  if (key != "LOOK") sendStatus();   // 눈동자 이동은 자주 오므로 응답 생략
}

// ---------------------- 화면 그리기 ----------------------
void updateTimer(uint32_t now) {
  if (!timerRunning) return;
  uint32_t dt = now - timerLastTick;
  timerLastTick = now;
  if (dt >= timerRemainMs) {
    timerRemainMs = 0; timerRunning = false; timerDone = true; timerDoneAt = now;
    setMode(M_TIMER);
    cycleAt = now + 15000;           // 알림 보여주는 동안 순환 멈춤
    sendLine("EV:TIMER_DONE");
  } else {
    timerRemainMs -= dt;
  }
}

void drawDdayMode(uint32_t now) {
  int idx[3], n = 0;
  for (int i = 0; i < 3; i++) if (ddays[i].used) idx[n++] = i;
  if (n == 0) { drawNoDday(u8g2); return; }
  if (!timeSynced) { drawNotSynced(u8g2); return; }
  if (now - ddayAt > 4000) { ddayPos++; ddayAt = now; }   // 4초마다 다음 디데이
  ddayPos %= n;
  const Dday& d = ddays[idx[ddayPos]];
  int32_t diff = daysFromCivil(d.y, d.m, d.d) - todayDays();
  drawDdayScreen(u8g2, d.title, diff, d.y, d.m, d.d, ddayPos, n);
}

void render(uint32_t now) {
  u8g2.clearBuffer();
  switch (mode) {
    case M_EYES: faceDraw(u8g2, face, now); break;
    case M_CLOCK: {
      struct tm t;
      if (localTime(t)) drawClockScreen(u8g2, t, use24h, now); else drawNotSynced(u8g2);
      break;
    }
    case M_DDAY: drawDdayMode(now); break;
    case M_TIMER:
      drawTimerScreen(u8g2, timerRemainMs, timerSetSec * 1000UL, timerRunning,
                      timerDone && now - timerDoneAt < 15000, now);
      break;
    case M_STOPWATCH: drawStopwatchScreen(u8g2, stopwatchMs(), swRunning, now); break;
    case M_TEXT: drawTextScreen(u8g2, message, textPage, textPageAt, now); break;
    default: break;
  }
  u8g2.sendBuffer();
}

// ---------------------- setup / loop ----------------------
void setupBLE() {
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(185);
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
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

void setup() {
  Serial.begin(115200);
  cmdQueue = xQueueCreate(CMD_QUEUE_LEN, CMD_MAX_LEN);

  prefs.begin("oled", false);
  loadSettings();

  u8g2.setI2CAddress(OLED_I2C_ADDR * 2);
  u8g2.setBusClock(400000);
  u8g2.begin();
  u8g2.enableUTF8Print();
  u8g2.setContrast(brightness);
  u8g2.setDisplayRotation(flipped ? U8G2_R2 : U8G2_R0);

  randomSeed(esp_random());
  faceScheduleBlink(face, millis());
  setupBLE();
  Serial.println("BLE advertising as " DEVICE_NAME);
}

void loop() {
  uint32_t now = millis();

  char cbuf[CMD_MAX_LEN];
  while (xQueueReceive(cmdQueue, cbuf, 0) == pdTRUE) {
    Serial.print("CMD "); Serial.println(cbuf);
    handleCommand(cbuf);
  }

  // 연결/끊김 처리
  if (bleConnected && !bleWasConnected) { bleWasConnected = true; }
  if (!bleConnected && bleWasConnected) {
    bleWasConnected = false;
    delay(300);
    server->startAdvertising();
  }
  if (sendConfigPending) { sendConfigPending = false; sendConfig(); }

  // 자동 순환
  if (cycleOn && (int32_t)(now - cycleAt) > (int32_t)cycleSec * 1000) {
    cycleAt = now;
    nextCycleMode();
  }

  updateTimer(now);
  faceUpdate(face, now);
  render(now);

  static uint32_t lastStatus = 0;
  if (bleConnected && now - lastStatus > 1000) { lastStatus = now; sendStatus(); }

  if (saveDirty && (int32_t)(now - saveAt) > 0) saveSettings();

  delay(12);
}
