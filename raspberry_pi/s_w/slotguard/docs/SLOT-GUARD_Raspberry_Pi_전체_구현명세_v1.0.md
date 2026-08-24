# 약SLOT-GUARD Raspberry Pi 전체 구현명세

문서 버전: v1.1  
작성 기준일: 2026-08-24  
대상: Raspberry Pi 소프트웨어·시스템 통합·시험 담당자  
대상 장치: Raspberry Pi 4 Model B Rev 1.5  
연동 장치: Waveshare 4inch RPi LCD (A) Rev 2.0, ATmega128A 제어보드, 헤드폰 오디오 출력  
소프트웨어 기준: SLOT-GUARD 앱 v0.3.0, Raspberry Pi OS 64-bit / Debian 13 Trixie  
연관 문서: `ATmega128A_UART_구현명세_v1.0`, `SLOT-GUARD_4inch_LCD_UIUX_기획서_v0.2`

## 1. 문서 목적과 범위

이 문서는 약SLOT-GUARD에서 Raspberry Pi가 담당하는 전체 소프트웨어와 시스템 통합 동작을 정의한다. UART만 분리해 설명하지 않고, 예약 입력부터 LCD 안내, ATmega 제어, 음성 재생, 데이터 저장, 시간 설정, 키오스크 부팅, 오류 복구와 운영 로그까지 하나의 흐름으로 연결한다.

문서의 기준은 현재 저장소에 구현된 코드다. 설계 의도와 현재 구현이 다른 항목은 `현재 구현상 주의점`에 별도로 표시한다.

이 문서가 다루는 범위는 다음과 같다.

1. Raspberry Pi, LCD, ATmega128A, 관리자 단말 사이의 역할 분담
2. Flask 앱, SQLite, UART 작업 스레드, 음성 재생기의 연관 관계
3. 관리자 웹과 장치 LCD 화면·API
4. 예약 상태머신과 블리스터 좌표 정책
5. SLOT-GUARD UART v2의 Pi 송수신 구현
6. 부팅, systemd 서비스, LightDM, Openbox, Chromium 키오스크
7. 시스템 시간, 전원, 권한, 보안 경계
8. 설치·배포·운영·로그·시험 기준

ATmega128A 내부의 EEPROM 중복 방지, 모터 구동 알고리즘과 센서 판정 세부값은 `ATmega128A_UART_구현명세_v1.0`의 책임 범위다.

## 2. 시스템 전체 구조

### 2.1 논리 구성

```text
관리자 스마트폰/PC
  └─ HTTP 5000 / 관리자 로그인
       └─ Flask app.py
            ├─ 관리자 웹: 예약·상태·시간 설정
            ├─ LCD API: 화면 상태·배출·설정·전원
            ├─ database.py → SQLite slotguard.db
            ├─ system_time_service.py → 제한된 시간 도우미
            ├─ VoiceAlertManager → cVLC → 헤드폰 출력
            └─ UartService → /dev/serial0 → ATmega128A
                                              ├─ X/Y 스테핑모터
                                              ├─ 배출 서보모터
                                              └─ 배출구 통과 센서

Raspberry Pi 로컬 X11 세션
  └─ LightDM 자동 로그인
       └─ slotguard-xsession
            ├─ 오디오 초기화
            ├─ Openbox
            └─ Chromium kiosk → http://127.0.0.1:5000/display
```

### 2.2 구성요소별 책임

| 구성요소 | 주 책임 | 입력 | 출력·연동 |
|---|---|---|---|
| Raspberry Pi | 예약·상태·화면·음성·통신의 중앙 조정 | 관리자 입력, LCD 터치, ATmega 응답 | DB 기록, LCD 화면, 음성, UART 명령 |
| Flask 앱 | HTTP 화면/API와 구성요소 연결 | HTTP 요청 | HTML, JSON, DB/UART/음성 호출 |
| SQLite | 영속 상태의 단일 기준 | 앱 트랜잭션 | 예약, 좌표, 요청번호, 설정, 이벤트 |
| UART 서비스 | 예약 판정과 ATmega 프로토콜 수행 | DB 상태, ATmega 프레임 | MOVE, DISPENSE, RESULT ACK, 상태 변경 |
| LCD 키오스크 | 환자용 현장 표시와 제한된 조작 | 1초 상태 폴링, 터치 | 배출, 확인, 설정, 전원 API |
| 관리자 웹 | 관리자용 예약·상태·시간 관리 | 스마트폰/PC 브라우저 | 예약 CRUD 일부, 전체 초기화, 시간 동기화 |
| 음성 관리자 | 복약 알림 반복과 소진 안내 | 일정 시작, 설정값, 중단 콜백 | cVLC 재생·중단 |
| 시간 서비스 | 현재 부팅에서 시간 신뢰 여부 관리 | 스마트폰 Unix 시간 | 시스템 시각 변경, boot ID 기록 |
| ATmega128A | 물리 동작과 센서 판정 | MOVE, DISPENSE | ACK, WAIT, RESULT, ERROR |
| Chromium 키오스크 | LCD용 웹 UI 전용 실행 환경 | 로컬 Flask 화면 | 전체화면 렌더링·터치 이벤트 |

### 2.3 책임 경계 원칙

- Pi는 시간, 예약, 요청번호, 재전송, 화면, 기록을 소유한다.
- ATmega는 모터의 실제 구동, 위치 도착, 센서 판정과 물리 중복 구동 방지를 소유한다.
- LCD는 예약을 만들거나 수정하지 않고 현재 흐름에 필요한 현장 동작만 제공한다.
- 관리자 웹은 예약과 시스템 시간을 관리하지만 직접 모터 명령을 만들지 않는다.
- 배출 성공은 센서 통과 확인이며 실제 복약 완료를 의학적으로 증명하지 않는다.
- 영속 상태의 기준은 SQLite이며, Pi 프로세스의 메모리 상태는 재시작 후 DB에서 복구한다.

## 3. 하드웨어와 외부 인터페이스

### 3.1 기준 하드웨어

| 항목 | 기준 | Pi 측 사용 목적 |
|---|---|---|
| 메인 컴퓨터 | Raspberry Pi 4 Model B Rev 1.5 | Flask, SQLite, UART, LCD, 음성, 키오스크 |
| LCD | Waveshare 4inch RPi LCD (A) Rev 2.0 | 480×320 가로형 화면 |
| 터치 | ADS7846/XPT2046 호환 저항막 | 단일 터치 조작 |
| 제어보드 | ATmega128A | 스테핑·서보·센서 제어 |
| UART 장치 | Pi `/dev/serial0` | ATmega USART 연결 |
| 오디오 | ALSA `sysdefault:CARD=Headphones` | 안내 음성 출력 |
| 저장소 | microSD의 로컬 파일시스템 | 앱, SQLite, 음성, 설정 |
| 네트워크 | 로컬 Wi-Fi 또는 Ethernet | 관리자 웹 접속과 최초 설치 |

### 3.2 인터페이스 요약

| 인터페이스 | Pi 자원 | 상대 | 용도 |
|---|---|---|---|
| UART TX/RX | GPIO 14/15, `/dev/serial0` | ATmega USART | MOVE·DISPENSE·응답 |
| SPI LCD | SPI0와 LCD 제어 GPIO | Waveshare LCD | 프레임버퍼 화면 출력 |
| 터치 입력 | ADS7846 입력 장치 | 터치패널 | Chromium 포인터 입력 |
| 오디오 | 헤드폰 ALSA 장치 | 스피커·앰프 | MP3 안내 음성 |
| HTTP | TCP 5000 | 로컬 Chromium, 관리자 단말 | 화면과 API |
| 로컬 파일 | 프로젝트 디렉터리 | SQLite·MP3·인증 파일 | 영속 데이터 |

### 3.3 UART 물리 연결 확인사항

- Pi TX는 ATmega RX로, Pi RX는 ATmega TX로 교차 연결한다.
- 양 장치는 공통 GND를 사용한다.
- Pi와 ATmega 사이의 논리 전압 호환 여부를 하드웨어 회로에서 확인한다.
- `/boot/firmware/config.txt`에 `enable_uart=1`이 적용되어야 한다.
- 로그인 콘솔이 `/dev/serial0`을 점유하지 않아야 한다.
- 앱 실행 사용자는 `dialout` 그룹에 포함되어야 한다.

## 4. Raspberry Pi 소프트웨어 구성

### 4.1 실행 파일과 역할

| 파일·디렉터리 | 역할 |
|---|---|
| `app.py` | Flask 화면/API, 인증, 음성 관리자, 구성요소 조립 |
| `database.py` | SQLite 스키마·마이그레이션·상태 전이·좌표·설정 |
| `uart_service.py` | UART v2 생성·파싱·예약 작업·재연결·재전송 |
| `system_time_service.py` | 스마트폰 시간 검증과 현재 부팅 시간 신뢰 상태 |
| `templates/` | 관리자 웹과 LCD HTML |
| `static/display.js` | LCD 화면 렌더링, 1초 폴링, 터치 동작 |
| `static/display.css` | 480×320 LCD 전용 스타일 |
| `static/style.css` | 관리자 웹 스타일 |
| `audio/` | 미리 생성된 복약·블리스터 안내 MP3 |
| `system/` | systemd, Xsession, LCD, 전원, 시간 설치 파일 |
| `tests/test_slotguard.py` | DB·UART·LCD API·시간·음성 자동시험 |
| `uart_loopback_test.py` | TX/RX 물리 루프백 전용 독립 시험 |

### 4.2 프로세스와 스레드

| 실행 단위 | 시작 주체 | 수명 | 주요 역할 |
|---|---|---|---|
| `slotguard.service` | systemd | 상시 | Flask Python 프로세스 실행·재시작 |
| Flask 메인 프로세스 | `app.py` | 서비스와 동일 | HTTP 0.0.0.0:5000 처리 |
| `slotguard-uart` 스레드 | 앱 시작 | 앱 종료까지 | UART 연결·수신·예약 0.5초 점검 |
| 음성 알림 스레드 | 일정 시작 | 반복 종료·중단까지 | 복약 MP3 반복 재생 |
| 단발 음성 스레드 | 소진 안내 | 파일 종료까지 | 블리스터 소진 MP3 재생 |
| LightDM X11 세션 | 부팅 자동 로그인 | 로그인 세션 동안 | Openbox와 Chromium 키오스크 실행 |

### 4.3 주요 의존성

| 패키지 | 버전 | 용도 |
|---|---|---|
| Flask | 3.1.3 | HTTP 화면과 API |
| pyserial | 3.5 | `/dev/serial0` 통신 |
| python-docx | 1.2.0 | 프로젝트 명세 DOCX 생성용 |
| Chromium | OS 패키지 | LCD 키오스크 브라우저 |
| cVLC | OS 패키지 | MP3 재생 |
| Openbox | OS 패키지 | 최소 X11 창 관리자 |

## 5. 부팅과 키오스크 실행

### 5.1 부팅 순서

```text
Pi 전원 인가
  → 커널이 SPI LCD·UART 오버레이 로드
  → systemd가 slotguard.service 시작
  → app.py가 인증 설정 로드
  → SQLite 스키마 생성·v5 마이그레이션
  → UART 작업 스레드 시작
  → Flask가 0.0.0.0:5000 수신
  → LightDM이 지정 사용자로 자동 로그인
  → slotguard-xsession 실행
  → 화면 보호·DPMS 비활성화
  → 오디오 장치 초기화와 Openbox 시작
  → /api/display-status 응답까지 대기
  → Chromium을 /display 전체화면 키오스크로 실행
```

### 5.2 systemd 서비스 정책

| 항목 | 값 |
|---|---|
| 서비스명 | `slotguard.service` |
| 작업 디렉터리 | 프로젝트 루트 |
| 실행 명령 | `.venv/bin/python app.py` |
| 재시작 | 항상 |
| 재시작 지연 | 3초 |
| 표준출력 | systemd journal |
| Python 버퍼링 | `PYTHONUNBUFFERED=1` |
| 실행 사용자 | 설치 시 지정한 자동 로그인 사용자 |

서비스가 비정상 종료되면 systemd가 3초 뒤 다시 시작한다. 재시작 후 UART 서비스는 SQLite에 남은 활성 일정을 다시 불러온다.

### 5.3 Chromium 키오스크 정책

- URL은 `http://127.0.0.1:5000/display`로 고정한다.
- `--kiosk`와 `--app`을 사용해 브라우저 메뉴를 숨긴다.
- 번역, 동기화, 확장 기능, 비밀번호 관리자, 첫 실행 화면을 비활성화한다.
- 사용자 데이터는 `$HOME/.config/slotguard-chromium`에 분리한다.
- 화면 보호, DPMS, 빈 화면 전환을 비활성화한다.
- Flask LCD 상태 API가 응답하기 전에는 Chromium을 시작하지 않는다.

### 5.4 LCD 드라이버와 터치

- 설치 스크립트는 Waveshare 오버레이 파일의 SHA-256을 확인한 후 설치한다.
- 기존 `/boot/firmware/config.txt`와 오버레이는 백업한다.
- SPI와 UART를 함께 활성화하고 LCD 회전을 90도로 설정한다.
- Xorg는 fbdev 드라이버와 ADS7846 터치 보정값을 사용한다.
- 터치 보정값과 실제 프레임버퍼 번호는 장치별 실기 검증 대상이다.

## 6. 관리자 웹

### 6.1 인증

- 최초 접속 시 `/setup`에서 관리자 아이디와 8자 이상 비밀번호를 만든다.
- 비밀번호는 Werkzeug 해시로 저장하고 평문으로 저장하지 않는다.
- 인증 파일 `slotguard_auth.json`은 권한 `0600`으로 저장한다.
- 세션 쿠키는 HttpOnly와 SameSite=Lax를 사용한다.
- 30분 동안 요청 활동이 없으면 세션을 만료시킨다.
- 관리자 API는 미설정, 미로그인, 세션 만료를 각각 오류로 반환한다.

### 6.2 관리자 화면 기능

| 화면·기능 | 경로 | 동작 |
|---|---|---|
| 초기 설정 | `/setup` | 최초 관리자 계정 생성 |
| 로그인 | `/login` | 관리자 인증 |
| 대시보드 | `/` | 시간, 일정 집계, 좌표, UART 상태 |
| 일정 관리 | `/schedules` | 일정 등록·조회·대기 일정 삭제 |
| 로그아웃 | `/logout` | 세션 삭제 |
| 장치 상태 API | `/api/status` | DB·UART·시간·일정 수 JSON |
| 시간 설정 API | `/api/system-time` | 스마트폰 시간으로 Pi 시각 설정 |

### 6.3 일정 등록 규칙

| 필드 | 규칙 |
|---|---|
| 약 이름 | 필수, 앞뒤 공백 제거, 1~30자 |
| 예정 날짜·시각 | `YYYY-MM-DD HH:MM`, 분 단위 |
| 허용시간 | 1~999999초 |
| 기본 허용시간 | 시간·분 미입력 시 1시간 |
| 같은 시각 | 기존 일정이 있으면 등록 거부 |
| 초기 상태 | `SCHEDULED` |
| 초기 좌표 | 미배정, 일정 도달 시 현재 슬롯을 배정 |

### 6.4 삭제와 전체 초기화

- 개별 삭제 SQL은 상태가 `SCHEDULED`인 일정만 삭제한다.
- `MOVING`, `READY_TO_DISPENSE`, `DISPENSING`은 물리 동작과 연결되어 개별 삭제하지 않는다.
- 완료·실패·미복약·통신 오류 기록도 개별 삭제하지 않는다.
- 현재 일정 관리 화면은 모든 행에 삭제 버튼을 표시하지만, 삭제 불가 상태에서는 아무 변경 없이 목록으로 돌아온다.
- 대시보드의 `일정 초기화`는 일정과 이벤트를 모두 삭제하고 좌표를 `00`으로 되돌린다.
- 전체 초기화는 ATmega에 취소 프레임을 보내지 않으므로 실제 모터가 완전히 정지한 시험·정비 상황에서만 사용한다.

## 7. LCD 키오스크 화면

### 7.1 기본 화면 구조

| 영역 | 기능 |
|---|---|
| 상단 | 브랜드, 장치 상태, 볼륨 테스트, 현재 시각 |
| 중앙 | 홈·작업 흐름·장치 상태·환경 설정 |
| 하단 | 홈, 장치 상태, 환경 설정 탭 |
| 확인 모달 | 블리스터 초기화·재시작·종료 2초 길게 누름 |
| 토스트 | API 성공·오류 피드백 |

LCD JavaScript는 `/api/display-status`를 1초마다 조회하고 서버 상태를 기준으로 전체 중앙 화면을 다시 렌더링한다. 진행 상태에서는 하단 메뉴 이동을 잠가 물리 동작 중 화면 이탈을 방지한다.

### 7.2 화면 우선순위

```text
1순위: 마지막 빈 슬롯의 BLISTER_EMPTY / EMPTY_BLISTER_CONFIRM
2순위: 확인되지 않은 FAILED / MISSED / COMM_ERROR
3순위: 활성 MOVING / READY_TO_DISPENSE / DISPENSING
4순위: 완료 후 5초 이내 DISPENSED / MANUALLY_COMPLETED
5순위: 현재 부팅 시간 미설정 TIME_REQUIRED
6순위: 일반 블리스터 소진 BLISTER_EMPTY
7순위: HOME 또는 사용자가 선택한 장치 상태·환경 설정
```

### 7.3 상태별 화면

| 화면 상태 | 표시 내용 | 사용자 동작 | 다음 상태 |
|---|---|---|---|
| HOME | 다음 약·시각·남은 시간·최근 기록·슬롯 | 탭 이동 | 상태 변화에 따라 자동 전환 |
| MOVING | 대상 약·슬롯·이동 중 경고. 빈 슬롯 재이동이면 전용 문구 | 없음 | WAIT 수신 시 준비 |
| READY_TO_DISPENSE | 복약 알림과 큰 약 배출 버튼 | 약 배출 | DISPENSING |
| DISPENSING | 서보 동작 중 접근 금지 | 없음 | RESULT 대기 |
| DISPENSED | 배출 확인 | 없음 | 5초 뒤 홈 |
| FAILED | 센서 미감지 안내 | 수동 완료 또는 복용하지 못함 | 완료 또는 확인 처리 |
| MANUALLY_COMPLETED | 수동 완료 기록 | 없음 | 5초 뒤 홈 |
| MISSED | 허용시간 종료 | 화면 확인 | 홈 |
| COMM_ERROR | UART 오류코드 | 화면 확인 | 홈 |
| TIME_REQUIRED | 관리자 웹 시간 설정 안내 | 없음 | 시간 설정 후 홈 |
| BLISTER_EMPTY | 새 블리스터 안내 | 2초 확인 후 초기화 | 빈 슬롯 복약이면 수동 확인, 아니면 HOME |
| EMPTY_BLISTER_CONFIRM | 새 블리스터 교체 후 복약 여부 | 대형 좌우 `수동복약` / `수동미복약` | 완료 또는 새 MOVE |
| DEVICE_ERROR | DB 조회 오류 | 없음 | 오류 해소 후 자동 복구 |

### 7.4 LCD에서 가능한 쓰기 동작

| API | 조건 | 동작 |
|---|---|---|
| `/api/display/dispense` | localhost, READY 상태 | DISPENSE 준비·즉시 전송 |
| `/api/display/manual-complete` | localhost, FAILED 상태 | 수동 완료와 좌표 1칸 증가 |
| `/api/display/empty-blister-choice` | localhost, 마지막 빈 슬롯 교체 완료 | 수동 완료 또는 동일 복약 재개 |
| `/api/display/acknowledge` | localhost, 실패 계열 상태 | 사용자 확인 기록 |
| `/api/display/settings` | localhost | 음성 반복·볼륨 저장 |
| `/api/display/test-volume` | localhost, 볼륨 1 이상 | 복약 음성 1회 재생 |
| `/api/display/reset-blister` | localhost, 활성 일정 없음 | 좌표와 소진 상태 초기화 |
| `/api/display/power` | localhost | 제한된 재시작·종료 실행 |

쓰기가 발생하는 LCD API는 요청 원격 주소가 `127.0.0.1` 또는 `::1`일 때만 허용한다.

## 8. 예약과 복약 상태머신

### 8.1 전체 전이

```text
SCHEDULED
  ├─ 허용시간 전에 예약 도달
  │    → 좌표 배정 → 요청번호 발급 → MOVING
  │         ├─ ACK 수신 → MOVING 유지, WAIT 대기
  │         └─ WAIT 수신 → READY_TO_DISPENSE
  │              ├─ 사용자가 약 배출 → DISPENSING
  │              │    ├─ RESULT의 R=1 → DISPENSED → 다음 좌표
  │              │    ├─ RESULT의 R=0 → FAILED → 좌표 유지
  │              │          ├─ 수동 복약 완료 → MANUALLY_COMPLETED → 다음 좌표
  │              │          └─ 복용하지 못함 → FAILED 확인 → 좌표 유지
  │              │    └─ RESULT의 R=2 → EMPTY_BLISTER_SLOT 기록 → 다음 좌표
  │              │          ├─ 다음 좌표 있음 → 새 MOVE → WAIT → 사용자 재배출
  │              │          └─ 마지막 좌표 14
  │              │                → 새 블리스터 초기화
  │              │                → 수동복약: MANUALLY_COMPLETED, 00→01
  │              │                → 수동미복약: 새 MOVE(00)로 동일 복약 재개
  │              └─ 허용시간 종료 → MISSED
  └─ 이미 허용시간이 지난 일정 발견 → MISSED

MOVING 또는 DISPENSING에서 허용시간 종료
  → COMM_ERROR
```

### 8.2 상태 정의

| 상태 | 의미 | 활성 여부 | 좌표 증가 |
|---|---|---|---|
| SCHEDULED | 미래 또는 처리 대기 예약 | 아니오 | 없음 |
| MOVING | MOVE 전송, ACK 또는 WAIT 대기 | 예 | 없음 |
| READY_TO_DISPENSE | 위치 도착, 사용자 배출 대기 | 예 | 없음 |
| DISPENSING | DISPENSE 전송, ACK 또는 RESULT 대기 | 예 | 없음 |
| DISPENSED | 센서가 정제 통과를 감지 | 종료 | 1칸 |
| FAILED | 서보 동작 후 센서 미감지 | 종료·확인 필요 | 없음 |
| MANUALLY_COMPLETED | 사용자가 직접 복용 확인 | 종료 | 1칸 |
| MISSED | 복약 허용시간 종료 | 종료·확인 필요 | 없음 |
| COMM_ERROR | 통신 단계가 허용시간까지 미완료 | 종료·확인 필요 | 없음 |

### 8.3 실패 결과 R=0 정책

```text
AT → Pi: RESULT|00000002|000
Pi: DISPENSE ACK 시각 기록
Pi: 일정 상태를 FAILED로 변경
Pi: error_code = NO_DROP_DETECTED
Pi → AT: ACK|00000002|RESULT
Pi: 현재 좌표 00 유지
LCD: 수동 복약 완료 / 복용하지 못함 선택 대기
```

- `R=0`만으로 Pi가 자동 재배출하지 않는다.
- `R=0`만으로 다음 슬롯으로 자동 이동하지 않는다.
- 사용자가 실제로 직접 복용했다고 확인한 경우에만 `MANUALLY_COMPLETED`로 바꾸고 좌표를 1칸 증가시킨다.
- `복용하지 못함`은 실패 화면만 확인 처리하며 좌표는 유지한다.
- 확인되지 않은 실패 결과가 있으면 다음 예약 처리를 차단한다.

### 8.4 빈 슬롯 결과 R=2 정책

```text
AT → Pi: RESULT|00000002|002
Pi: DISPENSE ACK 시각과 EMPTY_BLISTER_SLOT 이벤트 기록
Pi: 현재 좌표 00을 사용 완료로 처리하고 좌표를 01로 증가
Pi → AT: ACK|00000002|RESULT
Pi: 새 요청번호와 현재 남은 허용시간으로 MOVE|00000003|0|1|TTTTTT 전송
LCD: "현재 칸이 비어 있습니다. 다음 칸의 약으로 이동 중입니다."
AT → Pi: WAIT|00000003
LCD: 기존의 큰 약 배출 버튼 표시
```

- `R=2`는 압출기가 정상 동작했지만 해당 블리스터 슬롯이 비어 있음을 뜻한다.
- Pi는 빈 슬롯을 사용 완료로 기록하고 좌표를 정확히 한 칸 증가시킨다.
- RESULT ACK를 먼저 보낸 뒤 같은 복약 일정에 새 MOVE 요청번호를 발급한다.
- 새 MOVE의 `TTTTTT`에는 예약 마감까지 현재 남은 초를 저장하여 전송하고, 해당 MOVE 재전송에서는 같은 요청번호와 페이로드를 유지한다.
- WAIT 수신 후 자동 압출하지 않고 사용자가 기존 `약 배출` 버튼을 다시 눌러야 한다.
- 연속 `XY2`이면 각 빈 좌표를 기록하며 같은 절차를 `XY1`이 올 때까지 반복한다.
- 최종 `XY1`에서는 상태를 `DISPENSED`로 종료하고 `error_code`를 비운다.
- 재시도 중 `XY0`이면 기존 R=0 실패 정책을 그대로 적용한다.
- 이미 처리한 빈 슬롯 RESULT가 재수신되면 좌표를 다시 증가시키지 않고 RESULT ACK만 재전송한다.

마지막 좌표 `14`에서 `142`를 받은 경우에는 소진 안내 후 새 블리스터 초기화를 요구한다. 초기화하면 좌표를 `00`으로 바꾸고 화면 전체의 약 배출 영역을 좌우 절반으로 나눈 대형 `수동복약` / `수동미복약` 버튼을 표시한다.

- `수동복약`: 새 블리스터의 00번 약을 직접 복용한 것으로 기록하고 `MANUALLY_COMPLETED`, 좌표 `01`로 종료한다.
- `수동미복약`: 실패로 종료하지 않고 새 요청번호와 남은 허용시간으로 좌표 00의 MOVE를 보내 동일 복약을 계속한다. WAIT 후 기존 약 배출 버튼을 표시한다.
- 블리스터 교체 중 허용시간이 끝났다면 `수동미복약` 선택 시 새 MOVE를 보내지 않고 `MISSED / DOSE_WINDOW_EXPIRED`로 종료하며 좌표 00을 유지한다.

## 9. 예약 스케줄러와 시간 정책

### 9.1 작업 주기

- UART 작업 루프는 약 0.2초 간격으로 실행된다.
- 예약 판정은 최대 0.5초에 한 번 수행한다.
- UART 연결이 끊기면 5초 간격으로 재연결을 시도한다.
- ACK 미수신 MOVE와 DISPENSE는 10초 간격으로 같은 요청을 재전송한다.

### 9.2 처리 차단 조건

다음 조건에서는 새 예약을 시작하지 않는다.

- 현재 부팅에서 시스템 시간이 설정되지 않음
- 확인되지 않은 `FAILED`, `MISSED`, `COMM_ERROR`가 있음
- 이미 활성 일정이 있음
- 블리스터가 10칸 모두 사용되어 소진 상태임
- 예정 시각이 아직 도달하지 않음

### 9.3 허용시간

```text
deadline = scheduled_at + allowed_seconds
```

- 허용시간은 Pi가 첫 UART 프레임을 보낸 시각이 아니라 예약 시각부터 계산한다.
- 예약을 늦게 발견했을 때 이미 deadline을 지났으면 MOVE를 보내지 않고 `MISSED` 처리한다.
- READY에서 deadline이 지나면 `DOSE_BUTTON_TIMEOUT`으로 `MISSED` 처리한다.
- MOVING에서 deadline이 지나면 ACK 여부에 따라 `MOVE_ACK_TIMEOUT` 또는 `MOVE_READY_TIMEOUT`을 기록한다.
- DISPENSING에서 deadline이 지나면 ACK 여부에 따라 `DISPENSE_ACK_TIMEOUT` 또는 `RESULT_TIMEOUT`을 기록한다.

### 9.4 재시작 복구

- 앱 메모리의 활성 일정 ID는 재시작 시 사라진다.
- UART 작업 스레드는 DB에서 `MOVING`, `READY_TO_DISPENSE`, `DISPENSING` 일정을 다시 찾는다.
- 기존 MOVE·DISPENSE 요청번호와 좌표를 재사용한다.
- 물리 중복 구동 방지는 ATmega가 요청번호와 저장 상태를 이용해 보장해야 한다.
- Pi와 ATmega의 재부팅이 물리 동작 중 발생했으면 사용자의 기구 상태 확인이 필요하다.

## 10. Raspberry Pi UART 구현

### 10.1 직렬 설정

| 항목 | 값 |
|---|---|
| 포트 | `/dev/serial0` |
| 속도 | 9600 baud |
| 데이터 | 8 bit |
| 패리티 | None |
| 정지 비트 | 1 |
| 읽기 타임아웃 | 0.2초 |
| 쓰기 타임아웃 | 1초 |
| 문자 인코딩 | ASCII |
| 프레임 종료 | LF, 수신 CR 제거 |
| Pi 수신 버퍼 제한 | 미완성 프레임 256바이트 |

### 10.2 요청번호

- 형식은 `00000001`부터 `FFFFFFFF`까지의 대문자 16진수 8자리다.
- `00000000`은 사용하지 않는다.
- SQLite `request_sequence`에서 트랜잭션으로 증가시킨다.
- 최대값 다음에는 `00000001`로 순환한다.
- MOVE와 DISPENSE는 각각 새로운 요청번호를 사용한다.
- `XY2` 후 다음 좌표 MOVE와 새 블리스터 `수동미복약` 재개 MOVE도 각각 새로운 요청번호를 사용한다.
- 일정 삭제나 블리스터 초기화로 시퀀스를 되돌리지 않는다.

### 10.3 Pi 송신 프레임

```text
MOVE|RRRRRRRR|X|Y|TTTTTT\n
DISPENSE|RRRRRRRR|X|Y\n
ACK|RRRRRRRR|RESULT\n
```

| 프레임 | 생성 시점 | 재전송 종료 |
|---|---|---|
| MOVE | 예약 도달과 좌표 배정 후, 또는 빈 슬롯 후 재이동 | MOVE ACK 수신 또는 허용시간 종료 |
| DISPENSE | LCD 약 배출 터치 후 | DISPENSE ACK 수신 또는 허용시간 종료 |
| RESULT ACK | 유효 RESULT 수신 후 | 단발, 중복 RESULT에는 다시 전송 |

최초 MOVE의 `TTTTTT`는 일정의 설정 허용시간이다. `XY2` 후 다음 좌표로 이동하거나 새 블리스터에서 `수동미복약`으로 재개할 때는 예약 시각과 허용시간으로 계산한 현재 남은 초를 별도 `move_allowed_seconds`에 저장한다. 같은 MOVE 요청번호를 재전송할 때는 시간이 흘러도 저장된 값을 바꾸지 않는다.

### 10.4 Pi 수신 프레임

```text
ACK|RRRRRRRR|MOVE\n
ACK|RRRRRRRR|DISPENSE\n
WAIT|RRRRRRRR\n
RESULT|RRRRRRRR|XYR\n
ERROR|RRRRRRRR|ERROR_CODE\n
```

| 수신 | Pi 검증 | DB·화면 결과 |
|---|---|---|
| MOVE ACK | 활성 MOVE 요청번호·단계 | `move_ack_at` 기록 |
| WAIT | 활성 MOVE 요청번호 | READY 상태와 `ready_at` 기록 |
| DISPENSE ACK | 활성 DISPENSE 요청번호·단계 | `dispense_ack_at` 기록 |
| RESULT | 요청번호와 실제 XY 및 결과코드 0·1·2 일치 | 성공·실패 종료 또는 빈 슬롯 다음 좌표 재이동 |
| ERROR | 활성 MOVE·DISPENSE 요청번호 | `COMM_ERROR`, `AT_` 접두 오류 |

WAIT는 MOVE ACK가 먼저 도착하지 않았더라도 유효 요청번호이면 ACK를 내포한 것으로 처리하여 `move_ack_at`과 `ready_at`을 함께 기록한다.

### 10.5 수신 파싱과 오류 처리

- 수신 바이트를 내부 버퍼에 누적하고 LF 단위로 분리한다.
- 프레임 끝의 CR은 제거한다.
- ASCII가 아닌 데이터는 버리고 UART 오류로 기록한다.
- 256바이트를 넘도록 LF가 없으면 버퍼를 비우고 오류를 기록한다.
- 요청번호, 좌표, RESULT의 XYR, 오류코드 형식을 정규식과 범위 검사로 검증한다.
- RESULT의 R은 `0`, `1`, `2`만 허용한다.
- 알 수 없는 프레임은 상태를 변경하지 않고 `last_error`와 journal에 남긴다.
- 결과 좌표가 기대 좌표와 다르면 RESULT를 무시하고 일정은 `DISPENSING`에 유지한다.

### 10.6 중복과 재전송

- MOVE ACK 전까지 같은 요청번호와 전체 페이로드를 10초마다 재사용한다.
- DISPENSE ACK 전까지 같은 요청번호와 전체 페이로드를 10초마다 재사용한다.
- 완료된 요청의 RESULT가 다시 오면 DB와 좌표를 다시 변경하지 않고 RESULT ACK만 다시 보낸다.
- 다음 좌표로 진행한 뒤 이전 `XY2` RESULT가 다시 와도 `event_log`의 처리 요청번호를 확인하여 ACK만 다시 보낸다.
- ATmega는 같은 MOVE·DISPENSE 요청을 재수신해도 물리 모터를 다시 구동하지 않아야 한다.
- 현재 Pi 구현은 MOVE ACK를 받은 뒤 MOVE 재전송을 멈추므로, WAIT 유실 복구는 ATmega의 WAIT 재전송 또는 Pi 구현 보완이 필요하다.

### 10.7 구형 프로토콜 임시 호환

현재 파서는 요청번호가 없는 다음 구형 입력도 임시로 허용한다.

```text
ACK
WAIT
XYR
```

구형 프레임을 수신하면 상태 전이는 수행할 수 있지만 `ATmega가 요청번호 없는 구형 프로토콜을 사용 중입니다` 오류를 기록한다. 통합 완료 후에는 UART v2만 사용하는 것이 원칙이다.

## 11. 좌표와 블리스터 정책

### 11.1 슬롯 매핑

| 사용자 슬롯 | 내부 좌표 | 사용자 슬롯 | 내부 좌표 |
|---|---|---|---|
| 1 | 00 | 6 | 10 |
| 2 | 01 | 7 | 11 |
| 3 | 02 | 8 | 12 |
| 4 | 03 | 9 | 13 |
| 5 | 04 | 10 | 14 |

### 11.2 좌표 증가 규칙

```text
00 → 01 → 02 → 03 → 04 → 10 → 11 → 12 → 13 → 14
```

- 좌표는 예약 등록 시가 아니라 해당 예약이 실제 시작될 때 배정한다.
- 센서 성공 `DISPENSED`와 빈 슬롯 결과 `R=2`에서 자동으로 한 칸 증가한다.
- 실패 후 사용자의 `MANUALLY_COMPLETED`에서도 한 칸 증가한다.
- 마지막 빈 슬롯 교체 후 `수동복약`에서도 새 블리스터 좌표가 `00→01`로 증가한다.
- `R=0`의 `FAILED`, `MISSED`, `COMM_ERROR`는 현재 좌표를 유지한다.
- 좌표 증가와 일정 완료 상태 변경은 하나의 SQLite 트랜잭션으로 처리한다.
- 마지막 좌표 `14`가 완료되면 좌표는 `14`에 남고 `blister_exhausted=1`이 된다.

### 11.3 블리스터 초기화

- 새 블리스터 초기화는 좌표를 `00`, 소진 상태를 0으로 바꾼다.
- 미래 일정은 유지한다.
- `MOVING`, `READY_TO_DISPENSE`, `DISPENSING` 중에는 초기화를 거부한다.
- 10번째 슬롯 완료 후에는 초기화 전까지 새 MOVE를 보내지 않는다.
- LCD에서는 오동작 방지를 위해 2초간 확인 버튼을 누르게 한다.
- 마지막 슬롯의 `R=2`로 교체한 경우 초기화 직후 `수동복약` 또는 `수동미복약` 선택을 완료할 때까지 다음 예약을 차단한다.
- 이때 `수동미복약`은 좌표 00에서 동일 복약의 MOVE 흐름을 재개한다.

## 12. SQLite 데이터 모델

### 12.1 파일과 연결

- 기본 DB 파일은 프로젝트 루트의 `slotguard.db`다.
- 환경변수 `SLOTGUARD_DB_PATH`로 시험 DB 경로를 바꿀 수 있다.
- 연결 타임아웃은 5초다.
- 외래키 검사를 연결마다 활성화한다.
- 현재 스키마 버전은 `PRAGMA user_version = 5`다.

### 12.2 테이블

| 테이블 | 목적 | 주요 데이터 |
|---|---|---|
| `schedules` | 예약과 전체 처리 상태 | 약, 시각, 상태, 좌표, 요청번호, 단계별 시각, 오류 |
| `device_state` | 블리스터 위치 단일 상태 | 현재 X/Y, 소진 여부, 변경 시각 |
| `device_settings` | 음성 설정 단일 상태 | 반복 횟수, 볼륨 단계 |
| `request_sequence` | UART 요청번호 단일 상태 | 마지막 정수 값 |
| `event_log` | 상태·송수신·설정 감사 기록 | 일정 ID, 이벤트, 요청번호, 상세, 시각 |
| `system_time_state` | 현재 부팅 시간 설정 증빙 | boot ID, 설정 시각, 스마트폰 시간대 |

### 12.3 schedules 주요 필드

| 그룹 | 필드 |
|---|---|
| 예약 | `id`, `medicine_name`, `scheduled_at`, `allowed_seconds`, `status` |
| 좌표 | `x_coordinate`, `y_coordinate` |
| MOVE | `move_request_id`, `move_allowed_seconds`, `move_sent_at`, `move_ack_at`, `ready_at` |
| DISPENSE | `dispense_request_id`, `dispense_sent_at`, `dispense_ack_at` |
| 결과 | `result_at`, `completed_at`, `error_code`, `acknowledged_at` |

### 12.4 트랜잭션 원칙

- 요청번호 발급과 MOVE·DISPENSE 준비는 트랜잭션으로 묶는다.
- 완료 상태와 좌표 증가는 같은 트랜잭션으로 묶어 이중 증가를 방지한다.
- 중복 RESULT는 현재 상태 조건에 걸려 두 번째 좌표 증가가 발생하지 않는다.
- 이벤트 로그의 일정 외래키는 일정 삭제 시 NULL로 바뀐다.
- 레거시 DB는 시작 시 현재 v5 구조로 마이그레이션하며 기존 일정을 보존한다.

### 12.5 주요 이벤트

| 이벤트 | 발생 조건 |
|---|---|
| MOVE_PREPARED | MOVE 요청번호 준비 |
| MOVE_SENT | MOVE UART 쓰기 성공 |
| MOVE_ACK | 유효 ACK 수신 |
| READY_TO_DISPENSE | 유효 WAIT 수신 |
| DISPENSE_PREPARED | LCD 배출 요청 |
| DISPENSE_SENT | DISPENSE UART 쓰기 성공 |
| DISPENSE_ACK | 유효 ACK 또는 RESULT 수신 |
| DISPENSED | R=1 완료 |
| FAILED | R=0 또는 실패 처리 |
| EMPTY_BLISTER_SLOT | R=2 좌표·요청번호 기록 |
| EMPTY_BLISTER_MANUAL_TAKEN | 새 블리스터에서 수동복약 선택 |
| EMPTY_BLISTER_MANUAL_NOT_TAKEN | 새 블리스터에서 수동미복약 선택, MOVE 재개 |
| MISSED | 허용시간 종료 |
| COMM_ERROR | 통신 단계 오류 |
| RESULT_ACKNOWLEDGED | 사용자가 실패 화면 확인 |
| BLISTER_RESET | 새 블리스터 초기화 |
| DEVICE_SETTINGS_UPDATED | 음성 설정 저장 |

## 13. 음성 안내

### 13.1 음성 자산

| 파일 | 용도 | 현재 내용 |
|---|---|---|
| `audio/medicine_time.mp3` | 일정 시작 반복 알림 | 약 복용 시간과 복용 안내 |
| `audio/blister_empty.mp3` | 10번째 슬롯 완료 안내 | 새 블리스터 교체 안내 |

현재 두 파일은 미리 생성된 한국어 여성 음성 MP3다. 앱 실행 중에는 TTS를 새로 합성하지 않고 로컬 파일을 재생하므로 인터넷 연결이 필요하지 않다.

### 13.2 재생 흐름

- 일정이 `MOVING`으로 시작되면 복약 알림을 별도 스레드에서 재생한다.
- 반복 횟수는 0~10이며 기본값은 1이다.
- 반복 사이에는 음성 종료 후 5초를 둔다.
- 반복 0 또는 볼륨 0이면 자동 복약 알림을 시작하지 않는다.
- 사용자가 약 배출 버튼을 누르거나 일정이 실패·완료·시간초과되면 진행 중 알림을 중단한다.
- 블리스터 소진 음성은 설정된 볼륨으로 한 번 비동기 재생한다.
- 볼륨 테스트는 복약 음성을 한 번 재생하고 기존 음성 작업을 먼저 중단한다.

### 13.3 출력과 볼륨

- 재생기는 `/usr/bin/cvlc`다.
- ALSA 장치는 `sysdefault:CARD=Headphones`로 고정한다.
- 앱의 볼륨 단계 0~10을 cVLC gain 0.0~1.5에 매핑한다.
- WirePlumber 기본 출력 경로와 ALSA PCM은 설치·로그인 시 가능한 범위에서 100%로 맞춘다.
- 사용자 체감 볼륨은 앱 gain으로 조절하고 OS 믹서의 중복 감쇠를 방지한다.
- 오디오 장치 이름이 다른 Pi에서는 재생 명령과 설치 설정을 수정해야 한다.

## 14. 시스템 시간

### 14.1 시간 신뢰 모델

- Pi는 매 부팅마다 새 Linux boot ID를 가진다.
- DB에 저장된 boot ID와 현재 boot ID가 같고 설정 시각이 있어야 `time_ready`다.
- 이전 부팅에서 시간을 설정했더라도 재부팅 후에는 다시 시간 설정이 필요하다.
- 시간 준비 전에는 예약 판정과 UART 전송을 차단한다.

### 14.2 스마트폰 시간 설정

```text
관리자 브라우저
  → Date.now()와 브라우저 시간대 전송
  → Flask가 형식·범위·시간대 검증
  → sudo -n /usr/local/sbin/slotguard-set-time 실행
  → Linux CLOCK_REALTIME 변경
  → 현재 boot ID와 설정 정보를 SQLite에 기록
```

| 항목 | 규칙 |
|---|---|
| 허용 연도 | 2024~2099 |
| 장치 시간대 | Asia/Seoul |
| 소스 시간대 | 유효한 IANA 시간대, 최대 64자 |
| 도우미 제한 | 숫자 Unix 초 1개만 허용 |
| 실행 제한 | sudoers에 지정된 앱 사용자만 허용 |
| 실행 타임아웃 | 5초 |

설치 스크립트는 NTP를 끄고 장치 시간대를 Asia/Seoul로 설정한다. 네트워크 없이도 스마트폰 시간이 있으면 현재 부팅을 활성화할 수 있다.

## 15. 전원 관리와 시스템 권한

### 15.1 전원 동작

- LCD는 `poweroff`와 `reboot` 두 동작만 요청할 수 있다.
- Flask는 `/usr/local/sbin/slotguard-power`만 실행한다.
- 일반 사용자 실행 시 비대화형 `sudo -n`을 사용한다.
- sudoers는 정확한 helper 경로와 두 인수만 허용한다.
- LCD에서는 2초 길게 누른 후 요청하여 실수 터치를 줄인다.

### 15.2 설치 시 사용자 권한

| 그룹·권한 | 목적 |
|---|---|
| `dialout` | `/dev/serial0` 접근 |
| `video` | 프레임버퍼 접근 |
| `input` | 터치 입력 접근 |
| 시간 helper sudoers | 제한된 시스템 시각 설정 |
| 전원 helper sudoers | 제한된 재시작·종료 |
| 인증 파일 0600 | 관리자 정보 보호 |

## 16. HTTP API와 보안 경계

### 16.1 API 분류

| 분류 | 인증·접근 조건 | 대표 경로 |
|---|---|---|
| 관리자 HTML | 세션 로그인 | `/`, `/schedules` |
| 관리자 API | 세션 로그인 | `/api/status`, `/api/system-time` |
| LCD 읽기 | 현재 구현상 인증 없음 | `/display`, `/api/display-status` |
| LCD 쓰기 | localhost 주소만 허용 | `/api/display/*` POST |

### 16.2 LCD 상태 응답 주요 필드

| 필드 | 의미 |
|---|---|
| `app_version` | 앱 버전 |
| `screen` | LCD가 렌더링할 화면 상태 |
| `now`, `date` | Pi 현재 시각 |
| `time_ready` | 현재 부팅 시간 설정 여부 |
| `schedule` | 화면을 점유한 일정 |
| `active_schedule` | 활성 물리 일정 |
| `next_schedule` | 다음 SCHEDULED 일정 |
| `remaining_seconds` | 허용시간까지 남은 초 |
| `target_coordinate` | 현재 대상 X/Y |
| `used_coordinates` | 완료된 슬롯 목록 |
| `recent_records` | 오늘 최근 종료 기록 3건 |
| `settings` | 음성 반복·볼륨 |
| `device` | 앱·DB·UART·네트워크·음성 상태 |

### 16.3 보안 운영 원칙

- 서비스는 현재 `0.0.0.0:5000`에서 HTTP로 수신하므로 신뢰할 수 있는 로컬망에서만 운용한다.
- 라우터에서 TCP 5000을 인터넷에 포트포워딩하지 않는다.
- 관리자 비밀번호와 `slotguard_auth.json`을 외부에 복사하지 않는다.
- LCD 쓰기 API는 localhost 제한을 유지한다.
- 백업 파일에 DB와 인증 파일이 포함되는 경우 동일한 접근 통제를 적용한다.
- 현재 구현에는 TLS, 별도 CSRF 토큰, 요청 속도 제한이 없으므로 외부 공개 서버로 사용하지 않는다.

## 17. 오류와 복구 정책

| 상황 | Pi 상태·오류 | 화면 | 운영자 조치 |
|---|---|---|---|
| UART 포트 열기 실패 | DISCONNECTED | 장치 상태 확인 | 배선, 권한, serial0 확인 |
| MOVE ACK 없음 | MOVE_ACK_TIMEOUT | 통신 오류 | AT 수신과 응답 로그 확인 |
| ACK 후 WAIT 없음 | MOVE_READY_TIMEOUT | 통신 오류 | 모터 완료와 WAIT 송신 확인 |
| 배출 버튼 미터치 | DOSE_BUTTON_TIMEOUT | 미복약 | 사용자 확인 |
| DISPENSE ACK 없음 | DISPENSE_ACK_TIMEOUT | 통신 오류 | AT 서보 요청 수신 확인 |
| ACK 후 RESULT 없음 | RESULT_TIMEOUT | 통신 오류 | 센서 판정과 RESULT 확인 |
| RESULT R=0 | NO_DROP_DETECTED | 배출 실패 | 직접 복용 여부 선택 |
| RESULT R=2, 다음 슬롯 있음 | EMPTY_BLISTER_SLOT 이벤트 | 다음 약 이동 중 | WAIT 후 약 배출 재선택 |
| RESULT R=2, 마지막 슬롯 | EMPTY_BLISTER_SLOT | 블리스터 교체 | 초기화 후 수동복약 여부 선택 |
| 교체 중 허용시간 종료 후 수동미복약 | DOSE_WINDOW_EXPIRED | 미복약 | 화면 확인 |
| RESULT 좌표 불일치 | 일정 유지, UART 오류 | 배출 중 유지 | AT의 저장 좌표 확인 |
| AT ERROR | `AT_오류코드` | 통신 오류 | AT 명세에 따라 점검 |
| DB 읽기 오류 | DB-READ-ERROR | 장치 오류 | 파일·디스크·권한 확인 |
| 음성 파일 없음 | journal 경고 | 음성 없음 | MP3 배포 확인 |
| 시간 helper 실패 | HTTP 503 | 시간 미설정 | helper와 sudoers 확인 |
| 블리스터 소진 | 처리 차단 | 교체 안내 | 새 블리스터 후 초기화 |

실패 계열 화면은 사용자가 확인하기 전까지 다음 예약을 막는다. 오류를 단순히 숨기기 위해 DB 상태를 직접 수정하지 않고 LCD 확인 흐름 또는 정비용 전체 초기화를 사용한다.

## 18. 로그와 운영 점검

### 18.1 서비스 로그

```text
sudo journalctl -u slotguard.service -n 100 -f -o cat
```

UART 로그만 실시간으로 보려면 다음을 사용한다.

```text
sudo journalctl -u slotguard.service -f -o cat | grep --line-buffered '\[UART\]'
```

정상 로그 예시는 다음과 같다.

```text
[UART] 연결됨: /dev/serial0, 9600 baud, 8N1
[UART] Pi → AT: 'MOVE|00000001|0|0|003600'
[UART] AT → Pi: 'ACK|00000001|MOVE'
[UART] AT → Pi: 'WAIT|00000001'
[UART] Pi → AT: 'DISPENSE|00000002|0|0'
[UART] AT → Pi: 'RESULT|00000002|001'
[UART] Pi → AT: 'ACK|00000002|RESULT'
```

### 18.2 기본 점검 명령

```text
sudo systemctl status slotguard.service --no-pager
ls -l /dev/serial0
id
sqlite3 slotguard.db "SELECT id,status,move_request_id,dispense_request_id,error_code FROM schedules;"
```

### 18.3 점검 순서

1. `slotguard.service`가 active인지 확인한다.
2. `/dev/serial0` 심볼릭 링크와 권한을 확인한다.
3. 앱 사용자가 `dialout`, `video`, `input` 그룹인지 확인한다.
4. journal에서 UART 연결 로그를 확인한다.
5. 관리자 웹에서 Pi 시간을 설정한다.
6. 가까운 시각에 시험 일정을 등록한다.
7. MOVE, ACK, WAIT 순서를 확인한다.
8. LCD 배출 버튼 후 DISPENSE, ACK, RESULT, RESULT ACK를 확인한다.
9. DB 상태와 좌표가 예상대로 바뀌었는지 확인한다.

`uart_loopback_test.py`는 ATmega 통합시험이 아니라 Pi TX/RX 핀을 직접 연결하는 물리 루프백 시험이다. `slotguard.service`가 포트를 사용 중일 때는 실행하지 않는다.

## 19. 설치와 배포

### 19.1 준비

```text
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
sudo ./system/install_waveshare4_lcd.sh
sudo ./system/install_slotguard.sh <자동로그인사용자>
sudo reboot
```

### 19.2 설치 스크립트가 변경하는 시스템 영역

| 대상 | 내용 |
|---|---|
| `/etc/systemd/system/slotguard.service` | Flask·UART 서비스 |
| `/usr/local/bin/slotguard-xsession` | 키오스크 세션 |
| `/usr/share/xsessions/slotguard.desktop` | LightDM 세션 항목 |
| `/etc/lightdm/` | 자동 로그인과 세션 선택 |
| `/etc/X11/xorg.conf.d/` | LCD 프레임버퍼와 터치 보정 |
| `/etc/chromium/policies/managed/` | 로그인·번역·비밀번호 정책 |
| `/usr/local/sbin/slotguard-power` | 전원 helper |
| `/usr/local/sbin/slotguard-set-time` | 시간 helper |
| `/etc/sudoers.d/` | 제한된 전원·시간 권한 |
| 사용자 WirePlumber 설정 | 기본 출력 볼륨 정책 |
| `/boot/firmware/` | LCD 오버레이와 SPI·UART 설정 |

### 19.3 업데이트 원칙

- DB와 `slotguard_auth.json`을 먼저 백업한다.
- 진행 중인 복약 일정과 모터 동작이 없는지 확인한다.
- 코드와 음성 파일을 배포한다.
- 의존성이 변경됐으면 `.venv`를 갱신한다.
- 설치 파일이 변경됐으면 설치 스크립트를 다시 실행한다.
- `systemctl daemon-reload` 후 서비스를 재시작한다.
- LCD 캐시는 정적 파일의 앱 버전 쿼리값으로 갱신한다.
- UART 통합시험과 음성·터치 시험을 다시 수행한다.

## 20. 시험 수용 기준

### 20.1 자동시험

현재 자동시험은 29개이며 다음 범주를 포함한다.

- UART v2 프레임 생성·파싱
- MOVE·DISPENSE 요청번호 분리
- ACK 유실 재전송과 동일 요청번호 유지
- WAIT의 암시적 MOVE ACK 처리
- R=1 좌표 증가와 R=0 좌표 유지
- R=2 연속 빈 슬롯의 새 요청번호·남은 시간 MOVE와 최종 R=1 완료
- R=2 재이동 뒤 R=0에서 기존 실패·좌표 유지 정책 적용
- 처리한 R=2 RESULT 중복 수신 시 ACK만 재전송
- 수동 완료 좌표 1회 증가
- 중복 RESULT의 좌표 중복 증가 방지
- RESULT 좌표 불일치 무시
- 허용시간과 통신 오류 전이
- 시스템 시간 미설정 시 스케줄러 차단
- UART 부분 프레임 수신
- 10번째 슬롯 소진과 초기화
- 마지막 슬롯 R=2 후 초기화와 수동복약의 00→01 처리
- 마지막 슬롯 R=2 후 수동미복약의 좌표 00 MOVE 재개
- 블리스터 교체 중 허용시간 종료 시 MISSED와 MOVE 미전송
- 수동복약·수동미복약 대형 좌우 버튼 레이아웃
- 활성 일정 중 블리스터 초기화 차단
- 음성 설정 영속성과 음소거
- LCD localhost 쓰기 제한
- 스마트폰 시간과 boot ID
- 레거시 SQLite 마이그레이션

실행 명령:

```text
.venv/bin/python -m unittest discover -s tests -v
```

### 20.2 통합시험

| ID | 시험 | 통과 기준 |
|---|---|---|
| PI-01 | 전원 인가 | 서비스와 키오스크 자동 시작 |
| PI-02 | 시간 미설정 | UART 명령 없음, TIME_REQUIRED 표시 |
| PI-03 | 스마트폰 시간 | 현재 부팅 활성화, 시각 일치 |
| PI-04 | 관리자 인증 | 미로그인 관리 화면 차단 |
| PI-05 | 일정 등록 | 약·시각·허용시간 저장 |
| PI-06 | 동일 시각 등록 | 두 번째 일정 거부 |
| PI-07 | MOVE 정상 | ACK와 WAIT 후 배출 버튼 표시 |
| PI-08 | MOVE ACK 유실 | 같은 ID 재전송, 모터 1회 |
| PI-09 | DISPENSE 정상 | 다른 ID 사용, RESULT 처리 |
| PI-10 | DISPENSE ACK 유실 | 같은 ID 재전송, 서보 1회 |
| PI-11 | RESULT R=1 | 일정 완료와 좌표 1칸 증가 |
| PI-12 | RESULT R=0 | FAILED와 좌표 유지 |
| PI-13 | 수동 완료 | FAILED에서만 좌표 1칸 증가 |
| PI-14 | 복용하지 못함 | 확인 후 좌표 유지 |
| PI-15 | 중복 RESULT | RESULT ACK 재전송, 좌표 1회 증가 |
| PI-16 | 결과 좌표 불일치 | 결과 무시, 오류 로그 |
| PI-17 | UART 분리·복구 | DISCONNECTED 후 5초 재연결 |
| PI-18 | 서비스 재시작 | DB 활성 상태와 요청번호 복구 |
| PI-19 | 10개 슬롯 | 00부터 14까지 순서 보장 |
| PI-20 | 마지막 슬롯 | 소진 표시와 신규 MOVE 차단 |
| PI-21 | 블리스터 초기화 | 미래 일정 유지, 좌표 00 |
| PI-22 | 음성 반복 | 설정 횟수와 5초 간격 |
| PI-23 | 볼륨 0·5·10 | 음소거·기본·최대 동작 |
| PI-24 | LCD 터치 | 모서리·연속 탭·길게 누름 동작 |
| PI-25 | 네트워크 단절 | LCD·DB·UART·음성 지속 |
| PI-26 | 안전 종료 | helper를 통한 정상 종료 |
| PI-27 | RESULT R=2 | 빈 좌표 기록·1칸 증가, ACK 후 새 ID·남은 시간 MOVE |
| PI-28 | 연속 R=2 후 R=1 | 매번 사용자 재배출, 최종 DISPENSED와 error_code 해제 |
| PI-29 | 마지막 슬롯 R=2 | 교체 후 대형 수동복약·수동미복약 좌우 버튼 |
| PI-30 | 마지막 빈 슬롯 선택 | 수동복약은 00→01 완료, 수동미복약은 00 MOVE, 만료 시 MISSED |

## 21. 현재 구현상 주의점과 보완 권고

| ID | 현재 동작 | 영향 | 권고 |
|---|---|---|---|
| NOTE-01 | 일정 삭제는 SCHEDULED만 가능하지만 모든 행에 버튼 표시 | 삭제가 고장처럼 보임 | 삭제 불가 상태는 버튼 비활성화와 이유 표시 |
| NOTE-02 | 전체 일정 초기화가 활성 상태도 DB에서 삭제 | AT 동작과 Pi 상태 분리 가능 | 활성 일정 중 초기화 금지 또는 정비 모드 추가 |
| NOTE-03 | MOVE ACK 후 Pi가 MOVE 재전송 중단 | WAIT 유실 복구 경로 부족 | AT의 WAIT 반복 또는 Pi 재요청 정책 확정 |
| NOTE-04 | LCD 읽기 API가 인증 없이 LAN에 노출 | 약 이름·일정 정보 노출 가능 | localhost 제한 또는 별도 읽기 토큰 검토 |
| NOTE-05 | Flask 내장 서버와 HTTP 사용 | 외부 공개에 부적합 | 신뢰 LAN 유지, 필요 시 WSGI·TLS 프록시 |
| NOTE-06 | 관리자 POST에 별도 CSRF 토큰 없음 | 악성 페이지 요청 가능성 | CSRF 보호 추가 검토 |
| NOTE-07 | 오디오 장치명이 Headphones로 고정 | 장치명이 다르면 무음 | 설치 시 장치 탐지·설정화 |
| NOTE-08 | 터치 보정·fbdev 번호가 고정 | 장치 차이에서 좌표·화면 오류 | 실기기별 검증과 설치 후 진단 추가 |
| NOTE-09 | 구형 UART 프레임을 임시 수용 | 요청번호 중복 방지 약화 | AT v2 완료 후 호환 경로 제거 |
| NOTE-10 | 센서 성공은 복약 자체를 증명하지 않음 | 기록 해석 오해 가능 | UI와 보고서에서 배출 확인으로 표현 유지 |

## 22. 완전 동작 예시

### 22.1 정상 배출

```text
1. 관리자가 혈압약 09:00, 허용시간 1시간 등록
2. 09:00에 Pi가 현재 좌표 00을 일정에 배정
3. Pi가 MOVE 요청번호 00000001 발급
4. 음성: 복약 안내 시작
5. Pi → AT: MOVE|00000001|0|0|003600
6. AT → Pi: ACK|00000001|MOVE
7. AT가 X/Y 이동 완료
8. AT → Pi: WAIT|00000001
9. Pi가 READY_TO_DISPENSE로 변경
10. LCD에 약 배출 버튼 표시
11. 사용자가 약 배출 터치
12. Pi가 DISPENSE 요청번호 00000002 발급
13. Pi → AT: DISPENSE|00000002|0|0
14. AT → Pi: ACK|00000002|DISPENSE
15. AT가 서보 1회 동작, 센서 감지
16. AT → Pi: RESULT|00000002|001
17. Pi가 DISPENSED와 좌표 01을 한 트랜잭션으로 저장
18. Pi → AT: ACK|00000002|RESULT
19. LCD 성공 화면 5초 후 홈
```

### 22.2 센서 미감지

```text
AT → Pi: RESULT|00000002|000
Pi: FAILED / NO_DROP_DETECTED 저장
Pi: 좌표 00 유지
Pi → AT: ACK|00000002|RESULT
LCD: 수동 복약 완료 또는 복용하지 못함 선택

선택 A: 수동 복약 완료
  → MANUALLY_COMPLETED
  → 좌표 01

선택 B: 복용하지 못함
  → FAILED 확인 완료
  → 좌표 00 유지
```

### 22.3 빈 슬롯과 같은 복약 재시도

```text
AT → Pi: RESULT|00000002|002
Pi: 좌표 00의 EMPTY_BLISTER_SLOT 저장, 현재 좌표 01
Pi → AT: ACK|00000002|RESULT
Pi: 새 요청번호 00000003 발급, 남은 허용시간 003540 저장
Pi → AT: MOVE|00000003|0|1|003540
LCD: 현재 칸이 비어 있습니다. 다음 칸의 약으로 이동 중입니다.
AT → Pi: ACK|00000003|MOVE
AT → Pi: WAIT|00000003
LCD: 약 배출 버튼 표시
사용자가 약 배출 터치
Pi → AT: DISPENSE|00000004|0|1
AT → Pi: RESULT|00000004|011
Pi: DISPENSED, error_code 비움, 현재 좌표 02
Pi → AT: ACK|00000004|RESULT
```

마지막 슬롯이 빈 경우:

```text
AT → Pi: RESULT|00000002|142
Pi: EMPTY_BLISTER_SLOT 기록, blister_exhausted=1
Pi → AT: ACK|00000002|RESULT
LCD: 새 블리스터 교체·초기화 안내
사용자: 새 블리스터 초기화
Pi: 좌표 00, 소진 상태 해제
LCD: 수동복약 | 수동미복약 대형 좌우 버튼

선택 A: 수동복약
  → MANUALLY_COMPLETED
  → 좌표 01

선택 B: 수동미복약, 허용시간 남음
  → 새 요청번호로 MOVE|...|0|0|남은시간
  → WAIT 후 기존 약 배출 버튼

선택 B: 수동미복약, 허용시간 종료
  → MISSED / DOSE_WINDOW_EXPIRED
  → 좌표 00 유지, MOVE 미전송
```

### 22.4 MOVE 응답 없음

```text
Pi → AT: MOVE|00000001|0|0|003600
10초 후 같은 MOVE 재전송
10초 후 같은 MOVE 재전송
...
예약 시각 + 허용시간 도달
→ MOVE_ACK_TIMEOUT
→ COMM_ERROR
→ 음성 중단
→ LCD 확인 대기
```

## 23. 구현 완료 판단 기준

Raspberry Pi 측 구현은 다음 조건을 모두 만족할 때 통합 완료로 판단한다.

1. 전원 인가 후 서비스와 480×320 키오스크가 자동 실행된다.
2. 시간 설정 전에는 예약과 UART가 안전하게 차단된다.
3. 관리자 웹에서 예약·상태·시간 관리가 가능하다.
4. LCD가 모든 상태와 오류를 실제 DB·UART 상태에 맞게 표시한다.
5. UART v2 요청번호, 재전송, RESULT 중복 방지가 ATmega와 함께 동작한다.
6. `R=0`은 자동 좌표 이동이나 자동 재배출을 일으키지 않는다.
7. `R=1`, `R=2` 또는 수동 완료에서만 좌표가 정확히 한 칸 증가한다.
8. `R=2`는 새 요청번호·남은 시간 MOVE와 사용자 재배출을 거쳐 같은 복약을 계속한다.
9. 10번째 슬롯 이후 블리스터 소진이 유지된다.
10. 음성, 볼륨, 시간, 전원 helper가 재부팅 후에도 의도대로 동작한다.
11. 자동시험과 PI-01~PI-30 실기 통합시험 기록이 남는다.
12. NOTE-01~NOTE-10의 처리 여부가 릴리스 기록에 명시된다.
