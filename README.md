# 약속(藥SLOT) ATmega128A 상위설계서

# 0. 범위 / 전제

- 기반 문서: ATmega128A_UART_구현명세_v1.0 (Pi측 구현 완료, 프로토콜 확정)
- 대상: ATmega128A가 구현해야 하는 전체
    - 상태머신
    - UART 통신
    - 스테퍼(X/Y)
    - 서보(배출)
    - IR센서
    - 포토인터럽터(홈센서)
- 하드웨어(기구/전원)는 제외, 소프트웨어 구조만 다룸

---

# 1. 시스템 구성 & 하드웨어 리소스 배정

```markdown
Raspberry Pi 4                          ATmega128A
─────────────────                       ─────────────────
스케줄/DB/Flask/RTC 판단                 UART 파싱, 상태머신,
→ 언제·어디로 갈지 결정                  모터/센서 실시간 제어
```

| 장치 | 신호 | ATmega128 핀 | 모듈 물리 핀 번호 |
| --- | --- | --- | --- |
| UART (Pi 통신) | RXD0 | PE0 | 50 |
| UART (Pi 통신) | TXD0 | PE1 | 49 |
| 스테핑 모터 X축 (28BYJ-48+ULN2003) | IN1 | PA0 | 3 |
| 스테핑 모터 X축 (28BYJ-48+ULN2003) | IN2 | PA1 | 4 |
| 스테핑 모터 X축 (28BYJ-48+ULN2003) | IN3 | PA2 | 5 |
| 스테핑 모터 X축 (28BYJ-48+ULN2003) | IN4 | PA3 | 6 |
| 스테핑 모터 Y축 (28BYJ-48+ULN2003) | IN1 | PA4 | 7 |
| 스테핑 모터 Y축 (28BYJ-48+ULN2003) | IN2 | PA5 | 8 |
| 스테핑 모터 Y축 (28BYJ-48+ULN2003) | IN3 | PA6 | 9 |
| 스테핑 모터 Y축 (28BYJ-48+ULN2003) | IN4 | PA7 | 10 |
| 서보 모터 (MG996R) | PWM | PB5 (OC1A) | 16 |
| SEN0503 (IR1 빔센서, 낙하 감지) | Signal | PD2 (INT2) | 56 |
| SEN0503 (IR2 빔센서, 낙하 감지) | Signal | PE4 (INT4) | 46 |
| 홈 센서(SG255) X축  | Signal | PD0 (INT0) | 58 |
| 홈 센서(SG255) Y축 (신규) | Signal | PD1 (INT1) | 57 |

---

# 2. 상태 머신

```c
typedef enum {
    STATE_IDLE,
    STATE_MOVING,
    STATE_AWAITING_DISPENSE,
    STATE_DISPENSING,
    STATE_AWAITING_RESULT_ACK,
    STATE_RECOVERY_REQUIRED
} SystemState;
```

## 2.1. 부팅 시퀀스

```markdown

전원 인가
  → 주변장치 초기화 (UART / Timer1 / 인터럽트 / 포트)
  → STATE_RECOVERY_REQUIRED 진입 (무조건)
  → 홈 탐색 시작 (X축, Y축 동시 진행)
      ├─ 성공: 위치 (0,0) 확정 → STATE_IDLE → 명령 수신 개시
      └─ 어느 한 축이라도 스텝 한도 초과: 두 축 모터 정지 + 두 축 코일 전류 해제
                        → 수동으로 원점 이동 대기
                        → 두 축의 센서를 모두 감지 시 위치 (0,0) 확정 → STATE_IDLE
```

- Pi와 ATmega128A는 동일 전원에 의해 함께 재부팅되는 것을 전제로 함.
    - Pi 재부팅 시 이전 transaction을 복구하지 않고 새로운 sequence를 시작하며, ATmega128A 역시 부팅 시 항상 `STATE_RECOVERY_REQUIRED`에서 원점을 재확정한 후 `STATE_IDLE`로 진입함.
- 홈 센서가 감지되기 전에는 어떤 명령도 수행하지 않음.
- IDLE 분기 이전에 도착한 MOVE/DISPENSE 명령어는 `ERROR|reqid|RECOVERY_REQUIRED`로 응답
- 홈 탐색이 성공하면 자동으로 STATE_IDLE로 전이

## 2.2. 상태 전이표

| 상태 | 진입 동작 | 허용 입력 / 전이 |
| --- | --- | --- |
| STATE_IDLE | - | ▷새 MOVE → 검증→ACK 송신→MOVING

▷그 외 → 무시 |
| STATE_MOVING | 스텝 모터 동작 시작 | ▷같은 MOVE→ACK만 재송신(재시작 금지) 

▷도착→WAIT 송신→AWAITING_DISPENSE

▷다른 요청번호의 MOVE→BUSY

▷DISPENSE → NOT_READY

▷그 외 → 무시 |
| STATE_AWAITING_DISPENSE |  | ▷같은 MOVE→WAIT 재송신 

▷ 좌표일치 DISPENSE→ACK 송신→DISPENSING 

▷ 좌표불일치→COORD_MISMATCH 

▷MOVE→BUSY

▷TIMEOUT 수신 → ACK(TIMEOUT) → IDLE |
| STATE_DISPENSING | 1~3차 서보 배출 시퀀스 시작 (전 과정 IR 인터럽트 항시 활성화) | ▷같은 DISPENSE→ACK만 재송신(재시작 금지)  

▷어느 차수든 (이동/하강/상승 중) IR 감지 성공 시 즉시 서보 180도 복귀→RESULT(R=1) 송신→STATE_AWAITING_RESULT_ACK

▷ 3차 시도 0도 도달 직후 5초 대기 타이머 시작 (서보 180도 복귀 동작 병행)

▷ 3차 5초 대기 종료까지 IR 미감지 시→RESULT(R=2) 송신→STATE_AWAITING_RESULT_ACK

▷MOVE → BUSY |
| STATE_AWAITING_RESULT_ACK | - | ▷같은 DISPENSE→저장된 RESULT 재송신 

▷ RESULT ACK 수신→IDLE (결과 캐시 유지)

▷ACK 미수신 시 10초마다 RESULT 재전송, 최대 6회(누적 60초)

▷6회 초과 시 → 추가 프레임 송신 없이 STATE_IDLE로 강제 전이

▷MOVE → BUSY
 |
| STATE_RECOVERY_REQUIRED | 전원 인가 시 무조건 진입 → 홈 센서 기반 원점 탐색 시작 | ▷ [자동 복구] : X축, Y축 스텝 한계 내에서 원점 방향 이동 →  포토인터럽터 감지 → 위치(0,0) 저장 → 자동 IDLE

▷ [수동 복구] : 한계 스텝 초과 시 스텝 모터 정지 → 두 축 코일 전류 모두 해제(손으로 이동 가능) → 수동으로 원점 이동 → 포토인터럽터 감지 → 위치(0,0) 저장→ 자동 IDLE

▷ 복구 전 명령 수신 시 → `ERROR|reqid|RECOVERY_REQUIRED` 전송 |
| (모든 상태) | - | **[TIMEOUT]**
▷`AWAITING_DISPENSE` → `ACK|reqid|TIMEOUT` 송신 후 IDLE 전이
▷`IDLE` → 번호 대조 없이 `ACK|reqid|TIMEOUT` 재송신 (ACK 유실·리셋 대비 멱등 처리)
▷그 외 상태 → 무시(무응답)

[**ACK]**
▷`AWAITING_RESULT_ACK` 외 상태에서는 무시(무응답) |

---

# 3. UART 통신

## 3.1. UART 프로토콜

| 항목 | 값 |
| --- | --- |
| 포트 | Pi /dev/serial0 ↔ ATmega USART |
| 전송 속도 | 9600 baud |
| 데이터 | 8 bit |
| 패리티 | None |
| 정지 비트 | 1 |
| 흐름 제어 | 없음 |
| 문자 | ASCII 대문자 |
| 프레임 종료 | LF 0x0A(CR 허용 후 제거) |
| 최대 프레임 | 64 Byte |

## 3.2. 명령 및 응답

### 3.2.1. 요청 번호

- 형식: 대문자 16진수 8자리, 00000001~FFFFFFFF
    - 00000000은 사용하지 않음
- PI의 SQLite 시퀀스가 단조 증가 후 순환
    - Pi가 요청번호를 단조 증가시키고 기존에 사용했던 요청 번호는 재사용하지 않음

- MOVE와 DISPENSE는 서로 다른 요청 번호임.
- ACK가 없을 때 Pi는 10초마다 같은 요청 번호와 같은 전체 페이로드를 재전송.
- ATmega는 요청 번호만 비교하지 않고 명령 종류와 전체 페이로드 일치도 확인.
- **ID_CONFLICT 판정 대상은 `MOVE` / `DISPENSE` 두 명령뿐이다.** 같은 요청 번호로 좌표·허용시간이 다른 MOVE 또는 DISPENSE가 오면 동작하지 않고 ID_CONFLICT를 반환.
- 판정 순서(모든 상태 공통, 상태별 분기보다 선행):
    1. 프레임 포맷/문자/종료 검사 → 실패 시 `ERROR|INVALID_FORMAT` (요청번호 없이) 후 처리 종료
    2. 명령이 MOVE 또는 DISPENSE인 경우, 저장된 '직전 수신한 동일 명령 종류의 (요청번호, 페이로드)'와 비교
        - 요청번호는 같고 페이로드(좌표/허용시간)가 다르면 → `ERROR|reqid|ID_CONFLICT` 후 처리 종료 (상태·저장값 변경 없음)
        - 요청번호·페이로드가 모두 같으면 → 2.2절의 '같은 MOVE/DISPENSE' 규칙으로 진행
        - 처음 보는 요청번호면 → 신규 요청으로 2.2절 상태별 규칙으로 진행
    3. TIMEOUT/ACK는 이 판정에서 제외하고 곧바로 2.2절 상태별 규칙으로 진행
- **`TIMEOUT` 과 `ACK` 는 ID_CONFLICT 판정에서 제외한다.** 이 두 명령은 설계상 선행 명령의 요청번호를 그대로 재사용하므로(`TIMEOUT` ← MOVE 요청번호, `ACK` ← DISPENSE 요청번호), 판정 대상에 넣으면 명령 종류 불일치로 항상 충돌 처리되어 사이클이 종료되지 않음.

### 3.2.2. Pi→AT

- **MOVE** : 슬롯 좌표로 이동 지시
    
    ```c
    MOVE|RRRRRRRR|X|Y|TTTTTT\n
    ```
    
    - RRRRRRRR :  MOVE 요청번호
    - X | Y : 좌표
    - TTTTTT : 예약 기준 복약 허용시간(초)

---

- **DISPENSE** : 배출 지시
    
    ```c
    DISPENSE|RRRRRRRR|X|Y\n
    ```
    
    - RRRRRRRR :  DISPENSE 요청번호
    - X | Y : 좌표
    
    ⇒ DISPENSE 좌표는 이전 명령어  MOVE 좌표와 반드시 일치해야 함. 일치하지 않으면 서보 작동 X
    

---

- **ACK**(RESULT 응답용) : RESULT 수신 확인
    
    ```c
    ACK|RRRRRRRR|RESULT\n
    ```
    
    - RRRRRRRR : ACK 요청번호

---

- **TIMEOUT** : DISPENSE 없이 사이클 종료를 통보 (복약 허용시간 종료, 또는 원점 복귀 정리용)
    
    ```c
    TIMEOUT|RRRRRRRR\n
    ```
    
    - RRRRRRRR : 대상 MOVE 요청번호
    ⇒ AWAITING_DISPENSE 상태에서만 유효. AT는 ACK|RRRRRRRR|TIMEOUT으로 응답 후 IDLE 전환.  (모터는 이미 정지해있는 상태라 물리적으로 아무 것도 움직이지 않음)

---

### 3.2.3. AT→Pi

- **WAIT** : 목표 위치 도착 및 배출 가능 상태
    
    ```c
    WAIT|RRRRRRRR\n
    ```
    

---

- **RESULT** : 배출 결과 (X, Y 성공 여부)
    
    ```c
    RESULT|RRRRRRRR|XYR\n
    
    /*
    예시
    RESULT|00000002|001 : 0행 0열 압출 성공
    RESULT|0000000A|002 : 0행 0열 압출 실패
    RESULT|00000014|141 : 1행 4열 압출 성공
    RESULT|00000015|142 : 1행 4열 압출 실패
    */
    ```
    
    - X | Y : 실제 압출을 시도한(성공한) 좌표
    - R=1 : 1~3차 시도 중 어느 한 번이라도 IR 센서 감지 성공 → 즉시 종료
    - R=2 : 3차까지 모두 IR 센서 미감지 → 해당 슬롯 알약 소진(추정)으로 최종 실패 확정
    
    ⇒ AT는 RESULT를 보낸 후 Pi의 ACK|요청번호|RESULT를 기다린다. ACK가 유실되면 RESULT를 재전송할 수 있으며, Pi는 같은 결과를 중복 저장하거나 슬롯을 두 번 이동하지 않음
    
    ⇒ AT가 RESULT R=2를 보냈을 경우, ACK와 이어서 MOVE 명령어를 기다림
    

---

- **ERROR** : 오류
    
    ```c
    ERROR|RRRRRRRR|[CODE]\n
    ```
    
    ※ 오류 코드
    
    | 코드 | 조건 |
    | --- | --- |
    | `INVALID_FORMAT` | 필드 수·문자·종료 문자가 잘못됨 |
    | `INVALID_COORD` | X/Y 범위 오류 |
    | `INVALID_TIME` | 허용시간 범위 오류 |
    | `ID_CONFLICT` | 같은 요청번호에 다른 데이터 수신(MOVE & DISPENSE에 한함) |
    | `BUSY` | 다른 요청을 수행 중 |
    | `NOT_READY` | MOVE WAIT 이전 DISPENSE 수신 |
    | `COORD_MISMATCH` | DISPENSE 좌표와 READY 좌표 불일치 |
    | `RECOVERY_REQUIRED` | 동작 중 리셋되어 물리 상태를 확정할 수 없음 |
    | `STEPPER_ERROR` | 현재 하드웨어 구성에서는 일반 MOVE 중 자동 검출하지 않으며, MCU가 이동 실패를 명확히 판단할 수 있는 경우에만 사용 |
    | `SERVO_ERROR` | 서보 동작 실패를 별도로 검출 가능할 때 사용 |
    | `SENSOR_ERROR` | 센서 자체 고장과 단순 미감지를 구분할 수 있을 때 사용 |
- 예외 : `INVALID_FORMAT` 은 요청번호를 신뢰할 수 없는 상황이므로 요청번호 없이 송신
    
    ```c
    ERROR|INVALID_FORMAT\n
    ```
    

---

- **ACK**(MOVE/DISPENSE용) : 명령 수신 및 검증 완료
    
    ```c
    ACK|RRRRRRRR|MOVE or DISPENSE\n
    ```
    
    ⇒ ACK(MOVE) : 명령 형식과 수행 가능 여부를 확인하고 중복 방지 상태를 저장한 뒤, 스텝 모터를 움직이기 직전에 전송.
    

### 3.2.4. 전체 명령 흐름

```c
Pi→AT : MOVE|00000001|0|0|003600
AT→Pi : ACK|00000001|MOVE
(스텝모터 이동…)
AT→Pi : WAIT|00000001         

[타임아웃 발생 시]
Pi→AT : TIMEOUT|<MOVE_REQUEST_ID>\n
AT→Pi : ACK|<MOVE_REQUEST_ID>|TIMEOUT\n
  ── (STATE_IDLE 복귀)

(압출 버튼 클릭…)
Pi→AT : DISPENSE|00000002|0|0
AT→Pi : ACK|00000002|DISPENSE

── 1차 시도: (y-a) 이동 → 서보 하강(180→50→…→0) → 0도 도달 시 즉시 180도 복귀 → 복귀 완료 후 2차로 이동
   (이동/하강/상승 중 IR 감지 시 즉시 180도 복귀 후 RESULT R=1 전송 후 종료)

── 2차 시도: (y) 이동 → 서보 하강 → 0도 도달 시 즉시 180도 복귀 → 복귀 완료 후 3차로 이동
   (IR 감지 시 즉시 180도 복귀 후 RESULT R=1 전송 후 종료)

── 3차 시도: (y+a) 이동 → 서보 하강 → 0도 도달 시 즉시 180도 복귀 시작과 동시에 5초 감지 타이머 시작
   ├── 5초 이내 IR 감지 성공 → RESULT R=1 전송 후 종료
   └── 5초 경과 시까지 IR 미감지 → RESULT R=2 전송 후 종료 (최종 실패 확정)

[평상 시(아직 블리스터에 약이 있는 경우)] - 성공
AT→Pi : RESULT|00000002|001   ← R=1, 2차에서 성공
Pi→AT : ACK|00000002|RESULT
(STATE_IDLE 복귀)

[평상 시(아직 블리스터에 약이 있는 경우)] - 3회 모두 실패
AT→Pi : RESULT|00000002|002   ← R=2, 1~3차 모두 IR 미감지
Pi→AT : ACK|00000002|RESULT
Pi→AT : MOVE|00000003|0|1|002900 ← 다음 좌표의 MOVE 명령 재전송

[마지막 좌표에서 압출 시도 시]
  ── 마지막 슬롯(1,0) 배출이 끝나면, MOVE/WAIT/TIMEOUT으로 (0,0) 복귀를 처리
AT→Pi : RESULT|00000002|10R    ← (R=1 또는 R=2)
Pi→AT : ACK|00000002|RESULT
Pi→AT : MOVE|00000003|0|0|002900     ←   새 요청번호(home_req_id) 발급
AT→Pi : ACK|00000003|MOVE
((0,0)으로 스텝모터 이동…)
AT→Pi : WAIT|00000003      ← 이 시점에 물리적으로 (0,0) 도착 완료
Pi→AT : TIMEOUT|00000003   ← DISPENSE 없이 바로 (압출 의도 없음)
AT→Pi : ACK|00000003|TIMEOUT
             (AT 내부: STATE_AWAITING_DISPENSE → STATE_IDLE)
Pi : ACK|TIMEOUT 수신 후 새 블리스터 교체 화면 표시

***주의***
- home_req_id는 좌표가 (0,0)인 MOVE 요청.
    - AT 쪽에서 "원점 복귀 요청"을 구분하는 로직은 없음
    - 일반적인 MOVE 처리, TIMEOUT 처리와 동일한 상태 전이
- R=1이든 R=2든(성공/실패 무관) 이 절차는 동일하게 수행
```

---

# 4. 스텝 모터

| 항목 | 결정 내용 |
| --- | --- |
| 모터/드라이버 | 28BYJ-48 + ULN2003 |
| 핀맵 | X축 PA0–PA3, Y축 PA4–PA7 (PORTA nibble 분할 제어) |
| 구동 방식 | Full Drive (2상 여자, 4-step 시퀀스) |
| 속도 제어 방식 | 스텝 간 딜레이 조절로 속도 제어 → 3ms |
| 절대위치 계산좌표→스텝 변환 | 테이블로 저장(아래 코드 참고) |
| 위치 추적 | 스텝 카운트 기반 open-loop. 대기 상태에서 (0,0)으로 자동 복귀하지 않고 마지막 위치에 그대로 머뭄. RECOVERY_REQUIRED 시에는 홈 센서로 하드웨어 기반 위치 재확정. |
| `DISPENSE_SUB_OFFSET_STEPS` | 60 (3회 압출을 위한 스텝 모터 미세 이동) ⇒ 실측 후 조정 |
| `X_MIN_STEPS` | 0 |
| `X_MAX_STEPS` | 1420 |
| `Y_MIN_STEPS` | 0 |
| `Y_MAX_STEPS` | 4600+`DISPENSE_SUB_OFFSET_STEPS` |
- 구동 방식 : FULL DRIVE 시퀀스
    - 인접한 2개 코일을 동시에 여자시키는 4-step 시퀀스.
    - 토크 힘을 늘릴 수 있음.
    
    ```c
    // IN1,IN2,IN3,IN4 순서 (PA0-PA3 / PA4-PA7 nibble)
    const uint8_t FULL_DRIVE_SEQ[4] = {
        0b1100,
        0b0110,
        0b0011,
        0b1001
    };
    ```
    
- 좌표별 절대 스텝 테이블
    
    ```c
    // 원점(0)을 기준으로 한 절대 스텝 위치
    // X축: 0, 1  (2개)
    const int16_t X_STEP_TABLE[2] = { 0, 1420 };
    
    // Y축: 0, 1, 2, 3, 4  (5개)
    const int16_t Y_STEP_TABLE[5] = { 0, 1200, 2300, 3440, 4600 };
    ```
    

---

# 5. 서보 모터

| 항목 | 결정 내용 |
| --- | --- |
| 모델 | MG996R |
| 핀/PWM | PB5 (OC1A), Timer1 Fast PWM Mode 14, 50Hz, 16MHz, Prescaler 8 |
| 펄스폭 범위 | 500–2500μs |
| 각도→펄스 변환 | `Angle_To_Pulse()` 선형 변환, `PULSE_MIN_US`/`PULSE_MAX_US` 매크로 분리 |
| 대기 각도 | 180도 (DISPENSE 명령 수신 전까지) |
| 배출 시작 각도 | 50도 |
| 스텝 간 각도 | 5도씩 감소 |
| 정지 각도 | 0도 |
| 스텝 간 딜레이 | 500ms (timer tick - non-blocking 으로 구현 예정) |
| 압출 도중 IR 감지 시 | 즉시 서보 180도 복귀 후 성공(R=1) 처리 및 즉시 시퀀스 종료 |
| 0도 도달 후 | 0도 도달 즉시 180도 복귀 시작 (복귀 완료 후 즉시 다음 스텝 위치로 이동) |
| 알약 1개당 작동 위치 | 최대 3회 (우→중→좌), 성공 시 즉시 중단 |
| 5초 대기 타이머 | 5초 대기 타이머 |
- DISPENSING 내부 흐름 (우/중/좌 3회)

Y좌표 절대 스텝값 = `Y_STEP_TABLE[target_y])`  ⇒ 단순 좌표 설명을 위해 변수 y라고 가정

`DISPENSE_SUB_OFFSET_SETPS`: 60 (미세 이동 오프셋, 조정 필요) ⇒단순 좌표 설명을 위해 변수 a라고 가정

- DISPENSE 수신 시 진행 순서 (각 차수는 이전 차수 실패 시에만 진행):

| 차수 | 이동 위치 | 동작 |
| --- | --- | --- |
| 1차 | (y − a) [우측] | 이동 → 서보 하강(180→0) → 0도 도달 즉시 180도 복귀 → 복귀 완료 즉시 2차로 이동

(전 과정 IR 감지 시 즉시 180도 복귀 & RESULT R=1 전송 후 종료) |
| 2차 | (y) [중앙] | 이동 → 서보 하강(180→0) → 0도 도달 즉시 180도 복귀 → 복귀 완료 즉시 3차로 이동

(전 과정 IR 감지 시 즉시 180도 복귀 & RESULT R=1 전송 후 종료) |
| 3차 | (y + a) [좌측] | 이동 → 서보 하강(180→0) → **0도 도달 즉시 180도 복귀 시작 & 5초 타이머 시작**

▷ 5초 내 IR 감지 성공 → `RESULT(R=1)` 전송

▷ 5초 만료 시까지 미감지 → `RESULT(R=2)` 최종 실패 전송 |
- 서보 제어 각도 이동 방식
    - DISPENSE 명령 수신 시 50도로 즉시 점프
    - 이후 500ms 간격으로 5도씩 감소 제어
    - 0도 도달과 동시에 180도로 즉시 점프
- 경계 클램프
    - 각 차수의 목표 스텝은 아래와 같이 계산 후 클램프.
    
    ```c
    target = Y_STEP_TABLE[y] + offset      // offset = -a, 0, +a
    if (target < Y_MIN_STEPS) target = Y_MIN_STEPS;
    if (target > Y_MAX_STEPS) target = Y_MAX_STEPS;
    ```
    

---

# 6. IR 센서

| 항목 | 결정 내용 |
| --- | --- |
| 모델 | SEN0503 × 2 |
| 핀맵 | IR1 → PD2 (INT2), IR2 → PE4 (INT4) |
| 입력 방식 | 디지털 출력, 하드웨어 인터럽트, 내부 풀업 사용 |
| 판정 로직 | OR 로직 — 둘 중 하나만 감지되어도 배출 성공(R=1) |
| 판정 시점 | 판정 시점: DISPENSING 진입 시 두 센서 플래그 Clear. 1~3차 전 과정(모터 이동, 서보 하강/상승 포함) 동안 하드웨어 인터럽트(Falling Edge) 항시 활성화.
어느 순간이든 센서 감지 발생 시 즉시 서보 180도 복귀 후 성공(R=1) 판정 및 종료. 3차 0도 도달 후 5초 대기 타이머 만료 시까지 플래그 미갱신 시 실패(R=2) 판정. |
| 인터럽트 트리거 | Falling Edge |
| 실제 감지 기준 | HIGH→LOW |
- 해당 센서는 NPN 오픈 컬렉터로 내부 풀업을 사용함.
    - Idle 상태 :  HIGH(1)
    - 물체 감지 상태 : LOW(0)

---

# 7. 포토 인터럽터 (홈 센서)

| 항목 | 결정 내용 |
| --- | --- |
| 모델 | SG255 × 2 (투과형 포토인터럽터) |
| 핀맵 | X축 센서 → PD0 (INT0), Y축 센서 → PD1 (INT1) |
| 입력 방식 | 디지털 출력, 하드웨어 인터럽트, 외부 풀업(R2) 사용 |
| 극성 | 빔 클리어(정상) = LOW
빔 차단(감지) = HIGH |
| 인터럽트 트리거 | Rising Edge |
| 실제 감지 기준 | LOW→HIGH |
| 용도 | 부팅 시 RECOVERY_REQUIRED 진입하면 사람 개입 없이 자동으로 위치 재확정 |
| 탐색 방식 | X축과 Y축을 동시에 원점 방향으로 이동.
  1. X축 홈 센서 감지 → X축 이동 정지/코일 OFF → Y축은 계속 홈 방향 이동
  2. Y축 홈 센서 감지 → Y축 이동 정지/코일 OFF → X축은 계속 홈 방향 이동
X/Y 모두 홈 감지 시 즉시 정지 후 해당 지점은 원점 (0,0)으로 확정 |
| 감지 판정 방식 | **엣지 인터럽트 래치 + 레벨 폴링 병용.** 이미 원점에 정지한 상태로 부팅하면 신호가 HIGH로 고정되어 Rising Edge가 발생하지 않으므로, 탐색 시작 전 및 탐색 중 주기적으로 핀 레벨을 직접 읽어 판정. |
| 홈 방향 | X축 = 스텝 인덱스 감소 방향
Y축 = 스텝 인덱스 감소 방향 |
- 회로 구성
    - LED측: VCC --[220ohm]-- Anode | Cathode -- GND
    - TR측: VCC --[4.7kohm]-- Collector(=OUT, MCU 핀 연결) | Emitter -- GND
- 다루지 않는 실패 유형
    - 전기적 노이즈로 인한 오감지
    - 센서 감지 위치가 실제 (0,0)과 미세하게 어긋난 경우 → 정상 조립을 전제로 함

---

# 8. 슬롯 순회 경로

- 최초 위치: **(0,0)**
- 순회 순서: **(0,0)→(0,1)→(0,2)→(0,3)→(0,4)→(1,4)→(1,3)→(1,2)→(1,1)→(1,0)**
- [수정] (1,0)에서 **배출이 종료되면(R=1·R=2 무관)** ACK 수신 후 → Pi 가 MOVE(0,0) 요청을 보내 (0,0) 으로 복귀 → STATE_IDLE

| (0,4) | (0,3) | (0,2) | (0,1) | **(0,0)** |
| --- | --- | --- | --- | --- |
| (1,4) | (1,3) | (1,2) | (1,1) | (1,0) |

---

# 9. 전체 시나리오 흐름

| # | 주체 | 내용 | 상태 |
| --- | --- | --- | --- |
| 0-1 | AT | 전원 인가→ 주변장치 초기화 | STATE_RECOVERY_REQUIRED |
| 0-2 | AT | 홈 탐색 | STATE_RECOVERY_REQUIRED |
| 0-3 | AT | 홈 센서 감지 → 위치 원점 확정 → 명령 수신 개시 | STATE_IDLE |
| 1 | - | 대기 | STATE_IDLE |
| 2 | - | 예약된 복약 시간 도달 | - |
| 3 | Pi→AT | MOVE 명령 전송 | - |
| 4 | AT | ACK 전송 | - |
| 5 | AT | 절대좌표(X,Y)로 스텝모터 작동 | STATE_MOVING |
| 6 | AT | 목표 좌표 도착 | - |
| 7 | AT→Pi | WAIT 전송 | STATE_AWAITING_DISPENSE |
| 8 | Pi | 디스플레이에 압출 버튼 출력 (Pi 구현) | - |
| 9-1 | 사용자 | 허용 시간 내 버튼 클릭 O | →10-1번 |
| 9-2 | 사용자 | 허용 시간 내 버튼 클릭 X | →10-2번 |
| 10-1 | Pi→AT | DISPENSE 명령 전송 | →11-1번 |
| 10-2 | Pi→AT | TIMEOUT 명령 전송 | →11-2번 |
| 11-1 | AT→Pi | ACK|DISPENSE 전송 | STATE_DISPENSING(→12번) |
| 11-2 | AT→Pi | ACK|TIMEOUT 전송 | STATE_IDLE(→1번) |
| 12 | AT | 1차 압출 시도 ((y − a) 위치) | - |
| 13 | AT | 0도 도달 즉시 180도 복귀 시작 | - |
| 14-1 | AT | 감지 성공 | → 23번 |
| 14-2 | AT | 감지 실패 | → 15번 |
| 15 | AT | 중앙 좌표(y)로 스텝모터 이동 | - |
| 16 | AT | 2차 압출 시도 | - |
| 17 | AT | 0도 도달 즉시 180도 복귀 시작 | - |
| 18-1 | AT | 감지 성공 | → 23번 |
| 18-2 | AT | 감지 실패 | → 19번 |
| 19 | AT | (y+a) 좌표로 스텝모터 이동 | - |
| 20 | AT | 3차 압출 시도 | - |
| 21 | AT | 0도 도달 즉시 180도 복귀 시작 → 5초 타이머 시작 | - |
| 22-1 | AT | 5초 타이머 만료 전 감지 성공 | → 23번(R=1) |
| 22-2 | AT | 5초 타이머 만료 전 감지 실패 (3회 모두 실패=알약 없음) | → 23번(R=2) |
| 23 | AT→Pi | RESULT 전송 (R=1 또는 R=2) | STATE_AWAITING_RESULT_ACK |
| 24 | Pi→AT | ACK 전송 | - |
| 25 | AT | STATE_IDLE 상태 복귀 | STATE_IDLE |

**※ (1,0) 슬롯(순회 경로 마지막)의 경우, 25번 이후 Pi가 별도로 원점 복귀 절차(MOVE(0,0) → WAIT → TIMEOUT → ACK)를 진행함. AT는 이 판단을 하지 않으며, 3.2.5절 참고.**