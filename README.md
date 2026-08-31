# 약속(藥SLOT)은 지켜야지

> **기존 블리스터 포장을 유지한 사용자 확인형 복약 지원 시스템**  
> Team **약SLOT** · 제24회 임베디드SW경진대회 자유공모

약SLOT-GUARD는 약을 별도의 카트리지나 약통에 재포장하지 않고, **제조사의 2×5 블리스터 포장을 그대로 장착**하는 반자동 복약 지원 장치

복약 시간이 되면 장치가 대상 슬롯으로 자동 이동하고, 사용자가 LCD에서 배출을 승인하면 한 정을 압출합니다. 배출 결과는 IR 센서로 확인하여 장치 내부에 기록

- [시연 영상](https://youtu.be/20yo6nAbsLM)

## Contributors

| 이름 | GitHub | 담당 |
| --- | --- | --- |
| 이양배 | [twotwoship](https://github.com/twotwoship) | 팀장, 시스템 아키텍처 및 통합, Raspberry Pi Flask·SQLite, LCD UI/UX, 음성 안내, 전원부, 문서 |
| 신유지 | [shinyuji](https://github.com/shinyuji) | UART 프레임·파싱, ACK/WAIT/RESULT, 재전송 및 오류 처리, ATmega 통합시험 |
| 이호윤 | [nooyoh](https://github.com/nooyoh) | X/Y 스테핑모터, 서보 압출, 원점·위치 제어, IR 센서, 반복시험 |

공통 업무: 통합시험, 오류 분석, 시제품 개선, 시연 준비

## 개발 목표

1. 제조사 블리스터 포장을 그대로 사용
2. 2×5 좌표를 기반으로 정확한 슬롯 선택
3. 사용자 승인 후 한 정 압출
4. IR 센서를 이용한 실제 정제 통과 확인
5. 복약 결과와 장치 상태를 SQLite에 저장
6. Raspberry Pi와 ATmega128A의 역할 분리
7. 통신 오류가 약의 중복 배출로 이어지지 않도록 방지

## 핵심 특징

| 특징 | 설명 |
| --- | --- |
| 기존 포장 유지 | 약을 다시 분류하지 않고 제조사 블리스터를 그대로 사용 |
| 좌표 기반 슬롯 선택 | 2×5 블리스터의 각 슬롯을 X/Y 좌표로 관리 |
| 사용자 승인형 배출 | 목표 위치 도착 후 LCD에서 승인해야 약을 압출 |
| 실제 배출 감지 | IR 센서로 정제가 배출구를 통과했는지 확인 |
| 안전한 UART 통신 | Request ID와 ACK, 재전송, 중복 명령 차단을 적용 |
| 로컬 우선 동작 | 외부 인터넷 없이 Web, LCD, SQLite, UART, 음성 안내 동작 |

## 시스템 구성

```text
관리자 스마트폰
       │
       │ Local Web / HTTP :5000
       ▼
┌────────────────────────────────────┐
│           Raspberry Pi 4           │
│                                    │
│  Flask Web / LCD API               │
│  SQLite                            │
│  복약 일정 및 상태 관리            │
│  음성 안내                         │
│  UART 요청·재전송 관리             │
└────────────────┬───────────────────┘
                 │
                 │ UART 9600 bps / 8N1
                 ▼
┌────────────────────────────────────┐
│            ATmega128A              │
│                                    │
│  UART Frame / FSM                  │
│  X/Y 스테핑모터 제어               │
│  압출 서보 제어                    │
│  IR 정제 감지                      │
│  원점 센서 처리                    │
│  중복 물리 구동 방지               │
└────────────────┬───────────────────┘
                 │
                 ▼
       2×5 Blister / 배출구
```

### 역할 분리

| 구분 | 담당 기능 |
| --- | --- |
| Raspberry Pi 4 | 관리자 Web, LCD UI, SQLite, 일정 판단, 음성 안내, UART 요청·재전송, 시스템 상태 관리 |
| ATmega128A | X/Y 스테핑모터, 압출 서보, IR·원점 센서, 실시간 상태 제어, 중복 물리 구동 방지 |

## 복약 동작 과정

1. 보호자가 관리자 Web에서 약 이름, 복약 시간, 허용 시간을 등록
2. Raspberry Pi가 SQLite에 일정을 저장
3. 복약 시간이 되면 음성 안내를 시작하고 ATmega128A에 `MOVE`를 전송
4. ATmega128A가 X/Y 모터를 이용해 대상 슬롯으로 이동
5. 이동이 완료되면 `WAIT`를 Raspberry Pi에 반환
6. LCD의 약 배출 버튼이 활성화
7. 사용자가 배출 버튼을 누르면 Raspberry Pi가 `DISPENSE`를 전송
8. ATmega128A가 서보를 한 번 구동하여 정제를 압출
9. IR 센서가 정제 통과 여부를 확인하고 `RESULT`를 반환
10. Raspberry Pi가 결과와 처리 시각을 SQLite에 저장

## UART 안전 프로토콜

```text
Pi → AT : MOVE|00000001|0|0|003600
AT → Pi : ACK|00000001|MOVE
AT → Pi : WAIT|00000001

Pi → AT : DISPENSE|00000002|0|0
AT → Pi : ACK|00000002|DISPENSE
AT → Pi : RESULT|00000002|001
Pi → AT : ACK|00000002|RESULT
```

### 안전 설계

- 모든 물리 명령에는 8자리 Request ID를 사용
- ACK가 누락되면 10초마다 동일한 ID와 Payload를 재전송
- ATmega128A는 동일 요청을 다시 받아도 모터나 서보를 중복 금지
- `TIMEOUT`, `RESET`, `ERROR` 프레임으로 예외와 복구 상태를 관리
- 재부팅 전에 진행 중이던 물리 동작은 미실행

## 개발 환경

### Hardware

| 구분 | 구성 |
| --- | --- |
| Controller | Raspberry Pi 4, ATmega128A |
| Display | 4-inch RPi LCD, 480×320, XPT2046 Touch |
| Stepper Motor | 28BYJ-48 ×2 |
| Motor Driver | ULN2003 ×2 |
| Servo Motor | MG996R ×1 |
| IR Sensor | SEN0503 ×2 |
| Home Sensor | SG255 ×2 |
| Audio | ALSA Headphones 출력, Inkel Speaker |
| Communication | UART Level Converter, GPIO14/15 |
| Mechanical | 3D 프린팅 프레임, 압출 지그, 깔때기, 단일 배출구 |

### Software

| 영역 | 구성 |
| --- | --- |
| Raspberry Pi OS | 64-bit, Debian 13 Trixie |
| Application | Python, Flask 3.1.3 |
| Database | SQLite |
| Communication | pyserial 3.5, `/dev/serial0`, 9600 bps / 8N1 |
| Display | Chromium Kiosk, LightDM, X11 |
| Audio | cVLC, ALSA |
| Service | systemd `slotguard.service` |
| ATmega128A | Embedded C, Microchip Studio |

## 주요 기능

### 4-inch LCD

- 현재 시각 표시
- 다음 복약 일정과 남은 시간 표시
- 약 이름과 대상 슬롯 표시
- 위치 이동, 배출 준비, 배출 중, 성공·실패 상태 표시
- 최근 복약 기록 확인
- 음성 반복 횟수 및 볼륨 설정
- 블리스터 초기화
- 시스템 재시작 및 종료
- 운영 AP와 개발 Wi-Fi 모드 전환

LCD에서는 일정 등록이나 삭제를 수행하지 않고 복약 확인과 장치 설정만 수행

### 관리자 Local Web

- 최초 관리자 계정 설정 및 로그인
- 약 이름, 날짜, 시각, 복약 허용 시간 등록
- 예약 조회 및 삭제
- 동일 시각 중복 예약 방지
- 전체 복약 기록 확인
- UART 및 장치 상태 확인
- 스마트폰을 이용한 시스템 시간 동기화

### SQLite

다음 정보를 장치 내부에 저장합니다.

- 복약 일정
- 약 이름과 복약 허용 시간
- 현재 슬롯과 블리스터 상태
- MOVE/DISPENSE Request ID
- ACK/WAIT/RESULT 처리 시각
- 성공, 실패, 미복약, 통신 오류 결과
- 음성 반복 횟수와 볼륨 설정

## 프로젝트 구조

```text
yak_slot/
└── raspberry_pi/
    └── s_w/
        └── slotguard/
            ├── app.py
            ├── database.py
            ├── uart_service.py
            ├── system_time_service.py
            ├── requirements.txt
            ├── templates/
            ├── static/
            ├── audio/
            └── system/
```

| 파일 및 디렉터리 | 역할 |
| --- | --- |
| `app.py` | Flask Web, LCD API, 인증, 음성 및 전원 기능 |
| `database.py` | SQLite 스키마, 일정, 좌표, 상태 전이, 결과 저장 |
| `uart_service.py` | UART 송수신, ACK/WAIT/RESULT 처리, 재전송 |
| `system_time_service.py` | 스마트폰 기반 시스템 시간 동기화 |
| `templates/` | 관리자 Web 및 LCD HTML |
| `static/` | CSS, JavaScript, 버튼 효과음 |
| `audio/` | 복약 안내 음성 |
| `system/` | systemd, LCD, AP 및 권한 설치 도구 |


## 구현 검증

개발완료보고서 기준으로 다음 항목을 검증

| 시험 항목 | 판정 기준 | 결과 |
| --- | --- | --- |
| 예약 자동 실행 | 예약 시각에 대상 좌표 `MOVE` 전송 | PASS |
| 슬롯 위치 이동 | 목표 위치 도착 후 `WAIT` 반환 | PASS |
| 사용자 승인 제어 | `WAIT` 이전 배출 차단, 이후 버튼 활성화 | PASS |
| 정제 압출 | 사용자 승인 후 목표 슬롯에서 서보 구동 | PASS |
| 정제 통과 감지 | IR 감지 후 `RESULT` 반환 | PASS |
| 복약 결과 저장 | 결과와 처리 시각을 SQLite에 저장 | PASS |
| 중복 구동 방지 | 동일 Request ID 재수신 시 물리 재구동 없음 | PASS |
| 원점 복귀 | 원점 센서 감지 즉시 해당 모터 정지 | PASS |

## 차별성

기존 자동 복약기는 약을 장치 전용 카트리지, Bin 또는 Tray에 다시 넣어야 하는 경우가 많음

약SLOT-GUARD는 약을 장치에 맞추는 대신, **장치가 기존 블리스터의 대상 슬롯으로 이동**

1. 제조사 블리스터 포장을 그대로 사용
2. 재포장과 사전 분류 과정 최소화
3. X/Y 좌표를 이용한 대상 슬롯 직접 접근
4. 사용자 승인 후 한 정 압출
5. IR 센서를 이용한 실제 정제 통과 확인
6. Request ID 기반 중복 물리 구동 방지

## 발전 방향

1. **Adaptive Blister**
   - 카메라 기반 포장 규격 인식
   - 다양한 블리스터 크기 지원
   - 가변 슬롯 좌표 자동 생성

2. **Connected Care**
   - 보호자 모바일 UI
   - 미복약 및 장치 장애 원격 알림
   - 장기간 복약 이력 관리

3. **Productization**
   - Raspberry Pi와 MCU 통합 보드
   - 교체형 범용 블리스터 지그
   - 내구성 및 EMC 검증

4. **Care Platform**
   - 복수 사용자와 장치 통합 관리
   - 방문간호 데이터 연동
   - 돌봄기관 관제 Dashboard

## Git 및 PR 관리

### PR 규칙

- PR 제목에 작업 기간, 작업명, 작업자 이름을 가능한 한 포함
- 변경 목적과 주요 내용을 본문에 가능한 한 기록
- 검증 방법과 결과를 함께 가능한 한 포함
- 하드웨어 변경은 배선, 핀, 전원 및 기구부 영향을 설명

```text
26/07/19 ~ 26/07/23 스테핑 모터 제어 수정 - 이양배
```

### Commit 규칙

- 메시지는 `영역: 변경 내용` 형식으로 작성합니다.

```bash
git commit -m "web: 복약 일정 중복 등록 방지"
git commit -m "uart: RESULT 프레임 오류 처리 추가"
git commit -m "motor: X축 원점 복귀 로직 수정"
```
