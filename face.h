// face.h — 로봇 얼굴(눈·입·볼터치) 애니메이션
// 128x64 흑백 OLED용. U8g2 그리기 함수만 사용합니다.
#pragma once
#include <U8g2lib.h>
#include <math.h>
#include <stdint.h>

// 표정
enum Mood : uint8_t {
  MOOD_NORMAL, MOOD_HAPPY, MOOD_SAD, MOOD_ANGRY, MOOD_SLEEPY,
  MOOD_SURPRISED, MOOD_LOVE, MOOD_WINK, MOOD_LAUGH, MOOD_DIZZY, MOOD_COUNT
};
static const char* const MOOD_NAMES[MOOD_COUNT] = {
  "NORMAL", "HAPPY", "SAD", "ANGRY", "SLEEPY", "SURPRISED", "LOVE", "WINK", "LAUGH", "DIZZY"
};

// 눈 모양
enum EyeStyle : uint8_t { STYLE_SQUARE, STYLE_PILL, STYLE_RING, STYLE_CARTOON, STYLE_COUNT };

struct EyeShape {
  float w, h, r;      // 크기, 모서리 둥글기
  float lidTop;       // 위 눈꺼풀이 덮는 비율 (졸림)
  float lidSlope;     // +: 안쪽이 내려감(화남), -: 바깥쪽이 내려감(슬픔)
  float bottom;       // 아래를 둥글게 파내는 비율 (행복 ^ ^)
};

struct Face {
  Mood mood = MOOD_NORMAL;
  EyeStyle style = STYLE_SQUARE;
  bool mouth = true;
  bool blush = true;
  bool autoLook = true;

  float lookX = 0, lookY = 0, targetX = 0, targetY = 0;
  EyeShape cur = {28, 30, 8, 0, 0, 0};
  bool shapeInit = false;

  uint32_t nextLookAt = 0, nextBlinkAt = 0, manualUntil = 0;
  uint32_t blinkStart = 0;
  bool blinking = false;
  bool doubleBlink = false;
};

static const uint16_t FACE_BLINK_MS = 170;

// ---------------- 내부 도우미 ----------------
static inline int fRound(float v) { return (int)lroundf(v); }
static inline float fLerp(float a, float b, float t) { return a + (b - a) * t; }
static inline int fClamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 화면 밖 좌표는 u8g2에서 깨지므로 잘라서 그림
static void fBox(U8G2& g, int x, int y, int w, int h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > 128) w = 128 - x;
  if (y + h > 64) h = 64 - y;
  if (w > 0 && h > 0) g.drawBox(x, y, w, h);
}

static void fThickLine(U8G2& g, int x0, int y0, int x1, int y1, int t) {
  for (int k = -(t / 2); k <= t / 2; k++) {
    g.drawLine(x0, fClamp(y0 + k, 0, 63), x1, fClamp(y1 + k, 0, 63));
    g.drawLine(fClamp(x0 + k, 0, 127), y0, fClamp(x1 + k, 0, 127), y1);
  }
}

// 굵은 호(arc): 위쪽이 볼록한 ∩ 모양(닫힌 웃는 눈)
static void fArcUp(U8G2& g, int cx, int cy, int rx, int ry, int t) {
  for (int i = -rx; i <= rx; i++) {
    float u = (float)i / rx;
    int y = cy - fRound(ry * sqrtf(fmaxf(0.f, 1.f - u * u)));
    fBox(g, cx + i, y, 1, t);
  }
}

static void fHeart(U8G2& g, int cx, int cy, float s) {
  int r = fRound(s * 0.27f);
  if (r < 2) r = 2;
  g.drawDisc(cx - r + 1, cy - r / 2, r);
  g.drawDisc(cx + r - 1, cy - r / 2, r);
  g.drawTriangle(cx - 2 * r + 1, cy - r / 4 + 1, cx + 2 * r - 1, cy - r / 4 + 1, cx, cy + fRound(r * 1.9f));
}

static void fSpiral(U8G2& g, int cx, int cy, int R, float phase) {
  int px = cx, py = cy;
  for (int i = 0; i <= 60; i++) {
    float t = i / 60.0f;
    float a = t * 3.0f * 6.2832f + phase;
    float rr = t * R;
    int x = cx + fRound(cosf(a) * rr), y = cy + fRound(sinf(a) * rr);
    if (i) { g.drawLine(px, py, x, y); g.drawLine(px + 1, py, x + 1, y); }
    px = x; py = y;
  }
}

// 표정에 따른 눈 목표 모양
static EyeShape faceTarget(const Face& f) {
  EyeShape s;
  bool m = f.mouth;
  switch (f.style) {
    case STYLE_PILL:    s = {m ? 14.f : 16.f, m ? 30.f : 36.f, 7, 0, 0, 0}; break;
    case STYLE_RING:    s = {m ? 28.f : 32.f, m ? 28.f : 32.f, 14, 0, 0, 0}; break;
    case STYLE_CARTOON: s = {m ? 28.f : 32.f, m ? 30.f : 34.f, 14, 0, 0, 0}; break;
    default:            s = {m ? 28.f : 32.f, m ? 30.f : 36.f, 9, 0, 0, 0}; break;
  }
  switch (f.mood) {
    case MOOD_HAPPY:     s.bottom = 0.55f; break;
    case MOOD_SAD:       s.lidSlope = -0.5f; s.h *= 0.92f; break;
    case MOOD_ANGRY:     s.lidSlope = 0.55f; break;
    case MOOD_SLEEPY:    s.lidTop = 0.5f; break;
    case MOOD_SURPRISED: s.w *= 1.12f; s.h *= 1.12f; break;
    default: break;
  }
  return s;
}

// ---------------- 갱신 ----------------
static void faceScheduleBlink(Face& f, uint32_t now) { f.nextBlinkAt = now + 1800 + (uint32_t)random(0, 4200); }

static void faceBlink(Face& f, uint32_t now) { f.blinking = true; f.blinkStart = now; }

static void faceLook(Face& f, float x, float y, uint32_t now, uint32_t holdMs) {
  f.targetX = x; f.targetY = y; f.manualUntil = now + holdMs;
}

static void faceUpdate(Face& f, uint32_t now) {
  if (f.autoLook && now > f.manualUntil && now > f.nextLookAt) {
    int r = random(0, 10);
    if (r < 3) { f.targetX = 0; f.targetY = 0; }
    else { f.targetX = random(-100, 101) / 100.0f; f.targetY = random(-70, 71) / 100.0f; }
    f.nextLookAt = now + 900 + (uint32_t)random(0, 3200);
  }
  if (f.autoLook && !f.blinking && now > f.nextBlinkAt) {
    faceBlink(f, now);
    f.doubleBlink = random(0, 5) == 0;   // 가끔 두 번 깜빡
    faceScheduleBlink(f, now);
  }
  f.lookX += (f.targetX - f.lookX) * 0.3f;
  f.lookY += (f.targetY - f.lookY) * 0.3f;

  EyeShape t = faceTarget(f);
  if (!f.shapeInit) { f.cur = t; f.shapeInit = true; }
  const float k = 0.3f;   // 표정 전환 부드럽게
  f.cur.w = fLerp(f.cur.w, t.w, k);       f.cur.h = fLerp(f.cur.h, t.h, k);
  f.cur.r = fLerp(f.cur.r, t.r, k);       f.cur.lidTop = fLerp(f.cur.lidTop, t.lidTop, k);
  f.cur.lidSlope = fLerp(f.cur.lidSlope, t.lidSlope, k);
  f.cur.bottom = fLerp(f.cur.bottom, t.bottom, k);
}

// ---------------- 그리기 ----------------
// side: -1 = 화면 왼쪽 눈, +1 = 오른쪽 눈
static void faceDrawEye(U8G2& g, const Face& f, int cx, int cy, int side, float open, uint32_t now) {
  const EyeShape& s = f.cur;
  // 바라보는 쪽 눈이 살짝 커지는 원근 효과
  float persp = 1.0f + 0.10f * f.lookX * side;
  int w = fRound(s.w);
  int fullH = fRound(s.h * persp);
  int h = fRound(fullH * open);

  // 특수 표정
  if (f.mood == MOOD_LOVE) {
    float pulse = 1.0f + 0.08f * sinf(now / 140.0f);
    if (open < 0.35f) { fBox(g, cx - 12, cy - 1, 24, 3); return; }
    fHeart(g, cx, cy, 34 * pulse * (f.mouth ? 0.9f : 1.0f));
    return;
  }
  if (f.mood == MOOD_LAUGH) {   // > <
    int hw = 10, hh = 10;
    int tip = cx + side * -hw;   // 안쪽을 가리키는 꼭짓점
    int back = cx + side * hw;
    fThickLine(g, back, cy - hh, tip, cy, 3);
    fThickLine(g, tip, cy, back, cy + hh, 3);
    return;
  }
  if (f.mood == MOOD_DIZZY) {
    fSpiral(g, cx, cy, 13, now / 120.0f * side);
    return;
  }
  if (f.mood == MOOD_WINK && side > 0) {
    fArcUp(g, cx, cy + 6, w / 2, 9, 4);
    return;
  }
  if (f.mood == MOOD_HAPPY && s.bottom > 0.4f && f.style != STYLE_SQUARE && f.style != STYLE_PILL) {
    fArcUp(g, cx, cy + 6, w / 2, 10, 5);   // 동그란 눈 스타일은 ^ ^ 로
    return;
  }

  if (h < 5) { fBox(g, cx - w / 2, cy - 1, w, 3); return; }   // 감은 눈
  int x = cx - w / 2, y = cy - h / 2;

  switch (f.style) {
    case STYLE_RING: {
      int rx = w / 2, ry = h / 2;
      g.drawFilledEllipse(cx, cy, rx, ry);
      g.setDrawColor(0);
      if (rx > 4 && ry > 4) g.drawFilledEllipse(cx, cy, rx - 4, ry - 4);
      g.setDrawColor(1);
      int pr = rx / 3;
      int px = cx + fRound(f.lookX * (rx - pr - 5)), py = cy + fRound(f.lookY * (ry - pr - 5));
      if (ry > 8) g.drawDisc(px, py, pr);
      break;
    }
    case STYLE_CARTOON: {
      int rx = w / 2, ry = h / 2;
      g.drawFilledEllipse(cx, cy, rx, ry);
      g.setDrawColor(0);
      int prx = fRound(rx * 0.55f), pry = fRound(ry * 0.6f);
      int px = cx + fRound(f.lookX * (rx - prx - 2)), py = cy + fRound(f.lookY * (ry - pry - 2)) + 1;
      if (ry > 6) g.drawFilledEllipse(px, py, prx, pry);
      g.setDrawColor(1);
      if (ry > 8) {                       // 반짝이
        g.drawDisc(px - prx / 3, py - pry / 3, 3);
        g.drawDisc(px + prx / 3, py + pry / 3, 1);
      }
      break;
    }
    default: {   // 둥근 네모 / 알약
      int r = fRound(s.r);
      if (r > h / 2 - 1) r = h / 2 - 1;
      if (r > w / 2 - 1) r = w / 2 - 1;
      if (r < 1) fBox(g, x, y, w, h); else g.drawRBox(x, y, w, h, r);
      break;
    }
  }

  // 눈꺼풀(검정으로 덮기)
  g.setDrawColor(0);
  if (s.lidTop > 0.02f) fBox(g, x - 2, y - 2, w + 4, fRound(s.lidTop * h) + 2);
  if (fabsf(s.lidSlope) > 0.02f) {
    int inner = (side < 0) ? x + w + 1 : x - 1;
    int outer = (side < 0) ? x - 1 : x + w + 1;
    int drop = fRound(fabsf(s.lidSlope) * h);
    int top = y - 1;
    if (s.lidSlope > 0) g.drawTriangle(outer, top, inner, top, inner, top + drop);   // 화남
    else                g.drawTriangle(inner, top, outer, top, outer, top + drop);   // 슬픔
  }
  if (s.bottom > 0.02f) {
    int ry = fRound(h * 0.55f);
    int ecy = y + h - fRound(s.bottom * h) + ry;
    g.drawFilledEllipse(cx, fClamp(ecy, 0, 200), fRound(w * 0.75f), ry);
  }
  g.setDrawColor(1);
}

static void faceDrawMouth(U8G2& g, const Face& f, int cx, int my, uint32_t now) {
  switch (f.mood) {
    case MOOD_HAPPY: case MOOD_LOVE:
      g.drawDisc(cx, my - 2, 5, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      break;
    case MOOD_LAUGH:
      g.drawDisc(cx, my - 3, 7, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      break;
    case MOOD_SAD:
      g.drawCircle(cx, my + 3, 5, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      g.drawCircle(cx, my + 4, 5, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      break;
    case MOOD_ANGRY:
      g.drawCircle(cx, my + 4, 6, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      g.drawCircle(cx, my + 5, 6, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      break;
    case MOOD_SURPRISED:
      g.drawEllipse(cx, my, 3, 4);
      g.drawEllipse(cx, my, 2, 3);
      break;
    case MOOD_SLEEPY:
      fBox(g, cx - 3, my, 7, 2);
      break;
    case MOOD_DIZZY:
      for (int i = -7; i <= 7; i++) fBox(g, cx + i, my + fRound(sinf(i * 0.9f + now / 100.0f) * 1.5f), 1, 2);
      break;
    default:   // 기본 · 윙크: 작은 미소
      g.drawCircle(cx, my - 4, 5, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      g.drawCircle(cx, my - 3, 5, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      break;
  }
}

// 점무늬 볼터치
static void faceDrawBlush(U8G2& g, int cx, int cy) {
  for (int dy = -2; dy <= 2; dy++)
    for (int dx = -6; dx <= 6; dx++) {
      float e = (dx * dx) / 36.0f + (dy * dy) / 5.0f;
      int px = cx + dx, py = cy + dy;
      if (e <= 1.0f && ((px + py) & 1) == 0 && px >= 0 && px < 128 && py >= 0 && py < 64) g.drawPixel(px, py);
    }
}

static void faceDraw(U8G2& g, Face& f, uint32_t now) {
  float open = 1.0f;
  if (f.blinking) {
    uint32_t t = now - f.blinkStart;
    uint32_t total = f.doubleBlink ? FACE_BLINK_MS * 2 + 60 : FACE_BLINK_MS;
    if (t >= total) { f.blinking = false; f.doubleBlink = false; }
    else {
      if (t > FACE_BLINK_MS) t = (t < FACE_BLINK_MS + 60) ? 0 : t - FACE_BLINK_MS - 60;
      open = fabsf((float)t / FACE_BLINK_MS * 2.0f - 1.0f);
    }
  }

  bool m = f.mouth;
  int gap = (f.style == STYLE_PILL) ? 22 : 27;
  int baseY = m ? 26 : 32;
  float bob = sinf(now / 700.0f) * 0.8f;              // 숨 쉬듯 살짝 위아래
  int dx = fRound(f.lookX * 14);
  int dy = fRound(f.lookY * (m ? 4 : 7) + bob);

  faceDrawEye(g, f, 64 - gap + dx, baseY + dy, -1, open, now);
  faceDrawEye(g, f, 64 + gap + dx, baseY + dy, +1, open, now);

  if (f.blush) {
    int by = baseY + dy + (m ? 20 : 23);
    if (by <= 61) {
      faceDrawBlush(g, 64 - gap - 9 + dx, by);
      faceDrawBlush(g, 64 + gap + 9 + dx, by);
    }
  }
  if (m) faceDrawMouth(g, f, 64 + dx / 2, 52 + dy / 2, now);

  if (f.mood == MOOD_SLEEPY) {   // Zzz
    g.setFont(u8g2_font_5x8_tr);
    uint32_t ph = (now / 600) % 4;
    if (ph >= 1) g.drawStr(110, 18, "z");
    if (ph >= 2) g.drawStr(115, 12, "z");
    if (ph >= 3) g.drawStr(120, 6, "Z");
  }
}
