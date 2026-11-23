// screens.h — 시계 · 디데이 · 타이머 · 스톱워치 · 메시지 화면
#pragma once
#include <U8g2lib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// 작은 한글 글씨(12px)와 큰 한글 글씨(16px)
#define FONT_KO_SMALL u8g2_font_gulim12_t_korean2
#define FONT_KO_BIG   u8g2_font_unifont_t_korean2

static const char* const WEEK_KO[7] = {"일", "월", "화", "수", "목", "금", "토"};

static void drawCenterUTF8(U8G2& g, int y, const char* s) {
  int w = g.getUTF8Width(s);
  g.drawUTF8(w < 128 ? (128 - w) / 2 : 0, y, s);
}

static void drawCenterStr(U8G2& g, int y, const char* s) {
  int w = g.getStrWidth(s);
  g.drawStr(w < 128 ? (128 - w) / 2 : 0, y, s);
}

// 1970-01-01부터의 날짜 수 (Howard Hinnant 알고리즘)
static int32_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static void drawNotSynced(U8G2& g) {
  g.setFont(FONT_KO_SMALL);
  drawCenterUTF8(g, 28, "시간 정보가 없어요");
  drawCenterUTF8(g, 46, "앱을 연결해 주세요");
}

// ---------------- 시계 ----------------
static void drawClockScreen(U8G2& g, const struct tm& t, bool h24, uint32_t now) {
  int hour = t.tm_hour;
  if (!h24) { hour %= 12; if (hour == 0) hour = 12; }
  char hh[4], mm[4], ss[4];
  snprintf(hh, sizeof(hh), "%02d", hour);
  snprintf(mm, sizeof(mm), "%02d", t.tm_min);
  snprintf(ss, sizeof(ss), "%02d", t.tm_sec);

  g.setFont(u8g2_font_logisoso32_tn);
  int wh = g.getStrWidth(hh), wm = g.getStrWidth(mm);
  int total = wh + 10 + wm;
  int x0 = (128 - total) / 2 - 7;
  g.drawStr(x0, 41, hh);
  if ((now / 500) % 2 == 0) {            // 깜빡이는 콜론
    g.drawBox(x0 + wh + 3, 17, 4, 4);
    g.drawBox(x0 + wh + 3, 31, 4, 4);
  }
  g.drawStr(x0 + wh + 10, 41, mm);

  g.setFont(u8g2_font_profont12_tn);
  g.drawStr(x0 + total + 3, 41, ss);
  if (!h24) {
    g.setFont(FONT_KO_SMALL);
    g.drawUTF8(x0 + total + 2, 21, t.tm_hour < 12 ? "오전" : "오후");
  }

  char date[40];
  snprintf(date, sizeof(date), "%d월 %d일 %s요일", t.tm_mon + 1, t.tm_mday, WEEK_KO[t.tm_wday]);
  g.setFont(FONT_KO_SMALL);
  drawCenterUTF8(g, 61, date);
}

// ---------------- 디데이 ----------------
// diff = 목표일 - 오늘 (일)
static void drawDdayScreen(U8G2& g, const char* title, int32_t diff, int y, int m, int d,
                           int index, int count) {
  g.setFont(FONT_KO_SMALL);
  drawCenterUTF8(g, 12, title[0] ? title : "디데이");

  char big[16];
  if (diff > 0) snprintf(big, sizeof(big), "D-%ld", (long)diff);
  else if (diff == 0) snprintf(big, sizeof(big), "D-DAY");
  else snprintf(big, sizeof(big), "D+%ld", (long)-diff);
  g.setFont(diff == 0 || strlen(big) <= 5 ? u8g2_font_logisoso26_tr : u8g2_font_logisoso22_tr);
  drawCenterStr(g, 46, big);

  char date[32];
  int wd = (int)((daysFromCivil(y, m, d) % 7 + 11) % 7);   // 1970-01-01 = 목요일
  snprintf(date, sizeof(date), "%d.%02d.%02d (%s)", y, m, d, WEEK_KO[wd]);
  g.setFont(FONT_KO_SMALL);
  drawCenterUTF8(g, 62, date);

  if (count > 1) {   // 여러 개면 오른쪽 위에 점으로 표시
    for (int i = 0; i < count; i++) {
      int px = 128 - (count - i) * 5;
      if (i == index) g.drawBox(px, 1, 3, 3); else g.drawFrame(px, 1, 3, 3);
    }
  }
}

static void drawNoDday(U8G2& g) {
  g.setFont(FONT_KO_SMALL);
  drawCenterUTF8(g, 28, "디데이가 없어요");
  drawCenterUTF8(g, 46, "앱에서 추가해 주세요");
}

// ---------------- 타이머 ----------------
static void drawTimerScreen(U8G2& g, uint32_t remainMs, uint32_t totalMs, bool running, bool doneFlash, uint32_t now) {
  if (doneFlash) {
    bool on = (now / 300) % 2;
    if (on) g.drawBox(0, 0, 128, 64);
    g.setDrawColor(on ? 0 : 1);
    g.setFont(FONT_KO_BIG);
    drawCenterUTF8(g, 26, "시간 종료!");
    g.setFont(u8g2_font_logisoso16_tr);
    drawCenterStr(g, 54, "00:00");
    g.setDrawColor(1);
    return;
  }
  uint32_t sec = (remainMs + 999) / 1000;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));

  g.setFont(FONT_KO_SMALL);
  g.drawUTF8(0, 11, "타이머");
  const char* st = running ? "진행 중" : "멈춤";
  g.drawUTF8(128 - g.getUTF8Width(st), 11, st);

  g.setFont(u8g2_font_logisoso32_tn);
  drawCenterStr(g, 49, buf);

  g.drawRFrame(4, 55, 120, 8, 3);
  int fill = totalMs ? (int)(116.0f * (float)remainMs / totalMs) : 0;
  if (fill > 1) g.drawRBox(6, 57, fill, 4, 1);
}

// ---------------- 스톱워치 ----------------
static void drawStopwatchScreen(U8G2& g, uint32_t ms, bool running, uint32_t now) {
  uint32_t m = ms / 60000, s = (ms / 1000) % 60, cs = (ms / 10) % 100;
  char buf[12];
  g.setFont(FONT_KO_SMALL);
  g.drawUTF8(0, 11, "스톱워치");
  if (running && (now / 500) % 2) g.drawDisc(122, 6, 3);

  snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(m % 100), (unsigned long)s);
  g.setFont(u8g2_font_logisoso32_tn);
  int w = g.getStrWidth(buf);
  g.setFont(u8g2_font_logisoso16_tn);
  int wc = g.getStrWidth("00");
  int x0 = (128 - (w + 3 + wc)) / 2;
  g.setFont(u8g2_font_logisoso32_tn);
  g.drawStr(x0, 52, buf);
  snprintf(buf, sizeof(buf), "%02lu", (unsigned long)cs);
  g.setFont(u8g2_font_logisoso16_tn);
  g.drawStr(x0 + w + 3, 52, buf);
}

// ---------------- 메시지 (자동 줄바꿈, '|' = 줄바꿈) ----------------
#define TEXT_MAX_LINES 12
#define TEXT_LINE_BYTES 64

static int wrapUTF8(U8G2& g, const char* s, char lines[][TEXT_LINE_BYTES], int maxLines, int maxWidth) {
  int count = 0, len = 0;
  char cur[TEXT_LINE_BYTES] = "";
  for (int i = 0; s[i] && count < maxLines;) {
    uint8_t c = (uint8_t)s[i];
    int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    if (c == '|' || c == '\n') {
      strcpy(lines[count++], cur); cur[0] = 0; len = 0; i += 1; continue;
    }
    char test[TEXT_LINE_BYTES];
    if (len + n >= TEXT_LINE_BYTES) n = 0;   // 너무 긴 줄 보호
    memcpy(test, cur, len); memcpy(test + len, s + i, n); test[len + n] = 0;
    if (n && g.getUTF8Width(test) > maxWidth && len > 0) {
      char* sp = strrchr(cur, ' ');
      if (c == ' ') {                       // 공백에서 넘치면 그냥 줄바꿈
        strcpy(lines[count++], cur); cur[0] = 0; len = 0;
      } else if (sp && sp != cur) {         // 단어 단위로 줄바꿈
        *sp = 0;
        strcpy(lines[count++], cur);
        char rest[TEXT_LINE_BYTES];
        snprintf(rest, sizeof(rest), "%s", sp + 1);
        len = (int)strlen(rest);
        memcpy(cur, rest, len); memcpy(cur + len, s + i, n); len += n; cur[len] = 0;
      } else {                              // 공백 없는 긴 단어는 글자 단위
        strcpy(lines[count++], cur);
        memcpy(cur, s + i, n); cur[n] = 0; len = n;
      }
    } else if (n) {
      memcpy(cur, test, len + n + 1); len += n;
    }
    i += n ? n : 1;
  }
  if (count < maxLines && len) strcpy(lines[count++], cur);
  return count;
}

// page/pageAt 은 호출하는 쪽에서 보관 (긴 글은 2.5초마다 다음 쪽)
static void drawTextScreen(U8G2& g, const char* msg, int& page, uint32_t& pageAt, uint32_t now) {
  static char lines[TEXT_MAX_LINES][TEXT_LINE_BYTES];
  g.setFont(FONT_KO_BIG);
  int n = wrapUTF8(g, msg, lines, TEXT_MAX_LINES, 126);
  int pages = (n + 3) / 4;
  if (pages > 1 && now - pageAt > 2500) { page = (page + 1) % pages; pageAt = now; }
  if (page >= pages) page = 0;
  int start = page * 4;
  int shown = n - start < 4 ? n - start : 4;
  int top = (64 - shown * 16) / 2;
  for (int k = 0; k < shown; k++) drawCenterUTF8(g, top + 14 + k * 16, lines[start + k]);
}
