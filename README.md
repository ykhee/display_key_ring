# ESP32-C3 OLED 눈 리모컨 (BLE + 웹앱)

ESP32-C3 + SH1106 1.3인치 OLED(128×64, I2C)에 **눈 애니메이션·시계·타이머·스톱워치·메시지**를 띄우고,
스마트폰 웹앱으로 블루투스(BLE) 제어하는 프로젝트입니다.

```
firmware/esp32c3_oled_ble/esp32c3_oled_ble.ino   ← ESP32-C3에 업로드할 코드
docs/index.html                                  ← 스마트폰 웹앱 (파일 1개)
```

## 1. 배선

| OLED | ESP32-C3 |
|------|----------|
| VCC  | 3V3      |
| GND  | GND      |
| SDA  | GPIO 8   |
| SCL  | GPIO 9   |

> 보드마다 I2C 핀이 다를 수 있어요. 다른 핀에 연결했다면 코드 맨 위 `I2C_SDA`, `I2C_SCL` 값을 바꾸세요.

## 2. 펌웨어 업로드 (Arduino IDE 2.x)

1. **보드 매니저**에서 `esp32 by Espressif Systems` 설치
2. **라이브러리 매니저**에서 `U8g2` (by oliver) 설치 — BLE 라이브러리는 보드 패키지에 포함
3. 보드: `ESP32C3 Dev Module` 선택
   - ESP32-C3 Super Mini처럼 USB 칩이 없는 보드는 **Tools → USB CDC On Boot → Enabled** (시리얼 모니터용)
4. `esp32c3_oled_ble.ino` 열고 업로드
5. 화면에 `ESP32-Eyes / Waiting for BLE...`가 보였다가 눈이 나타나면 성공

**화면이 안 나올 때:** I2C 주소가 0x3D인 모듈이면 `OLED_I2C_ADDR`를 `0x3D`로 변경 / SDA·SCL 핀 확인

## 3. 웹앱 실행

웹 블루투스는 **https 주소**에서만 동작합니다. GitHub Pages로 무료 호스팅하는 방법:

1. 이 저장소 → **Settings → Pages**
2. Source: `Deploy from a branch`, Branch: 원하는 브랜치 / 폴더 `/docs` → Save
3. 잠시 후 표시되는 `https://<아이디>.github.io/<저장소>/` 주소를 스마트폰에서 열기

| 스마트폰 | 사용할 브라우저 |
|---------|----------------|
| 안드로이드 | **Chrome** (블루투스·위치 권한 켜기) |
| 아이폰 | Safari는 웹 블루투스 미지원 → **Bluefy** 앱 사용 |

[연결] → 목록에서 `ESP32-Eyes` 선택. 연결되면 스마트폰 시간이 자동으로 보드에 전송됩니다.
Chrome 메뉴의 **홈 화면에 추가**를 하면 앱처럼 쓸 수 있어요.

## 4. 기능

| 탭 | 기능 |
|----|------|
| 👀 눈 | 표정 8종(기본·행복·슬픔·화남·졸림·놀람·사랑·윙크), 드래그 패드로 눈동자 조종, 깜빡임, 자동 두리번 ON/OFF |
| 🕒 시계 | 폰 시간 동기화, 24/12시간제, 날짜·요일 표시 |
| ⏳ 타이머 | 프리셋(1~50분)·직접 입력, 시작/멈춤/리셋, 종료 시 OLED 깜빡임 + 폰 진동·알림음 |
| ⏱️ 스톱워치 | 1/100초 표시 |
| 💬 메시지 | 한글 메시지 표시(자동 줄바꿈, `|`로 줄바꿈), 밝기, 화면 180° 회전 |

## 5. 통신 규칙 (직접 기능을 추가할 때)

Nordic UART Service UUID(`6e400001-…`)를 사용하므로 **nRF Connect** 같은 BLE 앱으로도 테스트할 수 있어요.
RX 특성(`6e400002-…`)에 아래 텍스트를 쓰면 됩니다.

```
TIME:<epoch초>,<UTC오프셋분>   MODE:EYES|CLOCK|TIMER|STOPWATCH|TEXT
EYE:HAPPY   LOOK:-50,20   BLINK   AUTO:0
TIMER:SET,300   TIMER:START   TIMER:PAUSE   TIMER:RESET
SW:START   SW:STOP   SW:RESET
TEXT:안녕하세요   BRIGHT:128   FLIP:1   H24:0
```

새 기능은 펌웨어의 `handleCommand()`에 명령을, `loop()`의 `switch (mode)`에 화면을 추가하면 됩니다.

## 참고

- 한글은 U8g2의 `unifont_t_korean2` 폰트(자주 쓰는 한글 위주)를 사용해서, 드물게 쓰는 글자는 표시되지 않을 수 있어요.
- 보드에 시계용 배터리(RTC)가 없어서 전원이 꺼지면 시간이 초기화됩니다 → 앱을 다시 연결하면 자동 동기화.
