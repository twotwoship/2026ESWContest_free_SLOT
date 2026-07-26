# 약SLOT-GUARD ATmega128A UART 구현명세

문서 버전: v1.0  
작성 기준일: 2026-08-13  
대상: ATmega128A 펌웨어 개발팀  
연동 상대: Raspberry Pi 4 / SLOT-GUARD Flask·UART 서비스

## 1. 목적

예약 1건에서 다음 두 물리 동작을 분리하고, UART ACK 유실·재전송·양측 재부팅 상황에서도 모터가 중복 구동되지 않도록 한다.

1. MOVE: X/Y 스테핑모터를 대상 슬롯으로 이동
2. DISPENSE: 사용자가 LCD 버튼을 누른 뒤 서보모터로 한 슬롯의 한 정을 배출

## 2. UART 전기·프레임 규격

| 항목 | 값 |
|---|---|
| 포트 | Pi `/dev/serial0` ↔ ATmega USART |
| 전송 속도 | 9600 baud |
| 데이터 | 8 bit |
| 패리티 | None |
| 정지 비트 | 1 |
| 흐름 제어 | 없음 |
| 문자 | ASCII 대문자와 숫자 |
| 구분자 | `|` |
| 프레임 종료 | LF `0x0A`, 수신 시 CR은 허용 후 제거 가능 |
| 최대 프레임 | 64 bytes 이하 |

## 3. 요청번호

- 형식: 대문자 16진수 8자리, `00000001`~`FFFFFFFF`
- `00000000`은 사용하지 않는다.
- Pi의 SQLite 시퀀스가 단조 증가 후 순환한다.
- MOVE와 DISPENSE는 서로 다른 요청번호다.
- ACK가 없을 때 Pi는 10초마다 같은 요청번호와 같은 전체 페이로드를 재전송한다.
- ATmega는 요청번호만 비교하지 않고 명령 종류와 전체 페이로드 일치도 확인한다.
- 같은 요청번호에 다른 명령 또는 좌표가 오면 동작하지 않고 `ID_CONFLICT`를 반환한다.

## 4. 명령 및 응답

### 4.1 MOVE

```text
MOVE|RRRRRRRR|X|Y|TTTTTT\n
```

| 필드 | 의미 | 범위 |
|---|---|---|
| RRRRRRRR | MOVE 요청번호 | 8자리 HEX |
| X | 행 좌표 | 0~1 |
| Y | 열 좌표 | 0~4 |
| TTTTTT | 예약 기준 복약 허용시간(초) | 000001~999999 |

정상 순서:

```text
Pi → AT: MOVE|00000001|0|0|003600
AT → Pi: ACK|00000001|MOVE
AT: X/Y 스테핑모터 이동
AT → Pi: WAIT|00000001
```

ACK는 명령 형식과 수행 가능 여부를 확인하고 중복 방지 상태를 저장한 뒤, 스테핑모터를 움직이기 직전에 전송한다. WAIT는 목표 위치 도착과 서보 배출 가능 상태를 의미한다.

### 4.2 DISPENSE

```text
DISPENSE|RRRRRRRR|X|Y\n
```

정상 순서:

```text
Pi → AT: DISPENSE|00000002|0|0
AT → Pi: ACK|00000002|DISPENSE
AT: 서보모터 1회 동작
AT: 배출구 통과 센서 판정
AT → Pi: RESULT|00000002|001
Pi → AT: ACK|00000002|RESULT
```

DISPENSE 좌표는 마지막 READY 상태의 MOVE 좌표와 반드시 일치해야 한다. 일치하지 않으면 서보모터를 움직이지 않는다.

### 4.3 RESULT의 XYR

```text
RESULT|RRRRRRRR|XYR\n
```

| 필드 | 의미 |
|---|---|
| X | 실제 처리 행 0~1 |
| Y | 실제 처리 열 0~4 |
| R=1 | 서보 동작 후 배출구 통과 센서 감지 |
| R=0 | 서보 동작했으나 센서 감지 없음 |

예시:

- `RESULT|00000002|001`: 0행 0열 성공
- `RESULT|0000000A|000`: 0행 0열 실패
- `RESULT|00000014|141`: 1행 4열 성공
- `RESULT|00000015|140`: 1행 4열 실패

ATmega는 RESULT를 보낸 후 Pi의 `ACK|요청번호|RESULT`를 기다린다. ACK가 유실되면 RESULT를 재전송할 수 있으며, Pi는 같은 결과를 중복 저장하거나 슬롯을 두 번 이동하지 않는다.

### 4.4 ERROR

```text
ERROR|RRRRRRRR|ERROR_CODE\n
```

권장 오류코드:

| 코드 | 조건 |
|---|---|
| INVALID_FORMAT | 필드 수·문자·종료 문자가 잘못됨 |
| INVALID_COORD | X/Y 범위 오류 |
| INVALID_TIME | 허용시간 범위 오류 |
| ID_CONFLICT | 같은 요청번호에 다른 데이터 수신 |
| BUSY | 다른 요청을 수행 중 |
| NOT_READY | MOVE WAIT 이전 DISPENSE 수신 |
| COORD_MISMATCH | DISPENSE 좌표와 READY 좌표 불일치 |
| RECOVERY_REQUIRED | 동작 중 리셋되어 물리 상태를 확정할 수 없음 |
| STEPPER_ERROR | 위치 이동 실패 |
| SERVO_ERROR | 서보 동작 실패를 별도로 검출 가능할 때 사용 |
| SENSOR_ERROR | 센서 자체 고장과 단순 미감지를 구분할 수 있을 때 사용 |

오류코드는 영문 대문자, 숫자, `_`, `-`만 사용하고 최대 40자로 제한한다.

## 5. ATmega 상태머신

```text
IDLE
 └─ 새 MOVE → MOVE_ACCEPTED → MOVING → READY
      └─ 새 DISPENSE → DISPENSE_ACCEPTED → DISPENSING → RESULT_READY
           └─ RESULT ACK → RESULT_ACKED

새 블리스터 및 다음 예약의 새 MOVE → MOVE_ACCEPTED
```

| 상태 | 허용 입력 | 동작 |
|---|---|---|
| IDLE | 새 MOVE | EEPROM 저장, ACK, 스테핑 시작 |
| MOVING | 같은 MOVE | 모터 재시작 금지, ACK만 재전송 |
| READY | 같은 MOVE | WAIT 재전송 |
| READY | 좌표가 같은 새 DISPENSE | EEPROM 저장, ACK, 서보 1회 시작 |
| READY | 다른 MOVE/DISPENSE | BUSY 또는 COORD_MISMATCH |
| DISPENSING | 같은 DISPENSE | 서보 재시작 금지, ACK만 재전송 |
| RESULT_READY | 같은 DISPENSE | 저장된 RESULT 재전송 |
| RESULT_READY | RESULT ACK | 완료 표시, 결과 캐시는 유지 |
| 모든 상태 | 같은 ID+다른 데이터 | ID_CONFLICT, 동작 금지 |

## 6. 중복 방지 저장 규칙

### 6.1 저장 항목

최소한 다음 값을 EEPROM에 저장한다.

```text
magic/version
last_move_request_id
last_move_x, last_move_y, allowed_seconds
move_state: NONE / ACCEPTED / MOVING / READY
last_dispense_request_id
last_dispense_x, last_dispense_y
dispense_state: NONE / ACCEPTED / DISPENSING / RESULT_READY / RESULT_ACKED
last_result_xyr
record_crc
```

### 6.2 기록 순서

MOVE:

1. 프레임 검증
2. 요청번호/페이로드 중복 검사
3. EEPROM에 요청번호·좌표·`ACCEPTED` 기록과 CRC 확인
4. ACK 전송
5. `MOVING` 기록
6. 스테핑모터 시작
7. 위치 도착 후 `READY` 기록
8. WAIT 전송

DISPENSE:

1. READY 및 좌표 일치 확인
2. EEPROM에 요청번호·좌표·`ACCEPTED` 기록과 CRC 확인
3. ACK 전송
4. `DISPENSING` 기록
5. 서보모터를 정확히 1회 작동
6. 센서 판정
7. XYR과 `RESULT_READY` 기록
8. RESULT 전송

EEPROM 기록이 완료되지 않으면 모터를 시작하면 안 된다.

### 6.3 ATmega 리셋 복구

- READY 또는 RESULT_READY 상태였다면 저장된 WAIT 또는 RESULT를 재전송할 수 있다.
- MOVING 또는 DISPENSING 도중 리셋된 경우 같은 요청을 자동으로 다시 구동하지 않는다.
- 해당 요청번호에 `ERROR|요청번호|RECOVERY_REQUIRED`를 반환한다.
- 사용자의 물리 점검과 새로운 요청 전까지 서보 재동작을 금지한다.

EEPROM의 마지막 요청번호는 새 요청이 완료되어도 즉시 삭제하지 않는다. 다음 요청번호가 저장될 때까지 유지해야 직전 프레임 재전송을 중복 처리할 수 있다.

## 7. UART 수신 구현 권장

- RX 인터럽트에서는 고정 길이 링 버퍼에 바이트만 저장한다.
- 메인 루프에서 LF 기준으로 한 프레임을 추출한다.
- 64바이트를 넘으면 현재 프레임을 버리고 INVALID_FORMAT을 기록한다.
- `strtok` 사용 시 재진입성과 빈 필드를 주의하거나 고정 파서를 사용한다.
- 숫자 필드는 모든 문자가 범위 내 숫자인지 확인한 뒤 변환한다.
- 모터 동작 중에도 UART 수신과 ACK/상태 재전송이 가능해야 한다.
- 모터 제어를 긴 blocking delay로 구현해 UART 버퍼를 넘치게 하면 안 된다.

## 8. 타이밍 정책

- Pi 재전송 주기: 10초
- Pi 재전송 종료: 예약 시각 + 허용시간
- ATmega ACK: 유효 명령 수신·EEPROM 기록 후 가능한 즉시
- WAIT: 실제 X/Y 위치 도착 후
- RESULT: 서보 동작과 센서 판정이 끝난 후
- 센서 감지 창·디바운스 값은 기구 통합시험으로 확정하고 상수로 문서화한다.

ATmega는 Pi의 벽시계를 신뢰할 필요가 없다. 허용시간 종료 판정과 신규 전송 중단은 Pi가 담당한다.

## 9. 안전 규칙

1. 동일 요청번호로 같은 모터를 두 번 작동하지 않는다.
2. MOVE READY 이전에 서보를 작동하지 않는다.
3. DISPENSE 좌표가 READY 좌표와 다르면 동작하지 않는다.
4. 서보 동작 중 다른 DISPENSE 요청은 BUSY 처리한다.
5. R=0은 센서 미감지이며 ATmega가 자동으로 서보를 다시 작동하지 않는다.
6. RECOVERY_REQUIRED에서도 자동 재구동하지 않는다.
7. 요청번호·상태·결과가 EEPROM CRC 검증에 실패하면 모터를 금지한다.

## 10. 통합시험 수용 기준

| ID | 시험 | 통과 기준 |
|---|---|---|
| AT-01 | 10개 좌표 MOVE | 좌표 전수 WAIT 수신 |
| AT-02 | MOVE ACK 유실 | 같은 ID 재수신에도 스테핑 1회 |
| AT-03 | WAIT 유실 | 같은 MOVE ID에 WAIT 재전송, 재이동 없음 |
| AT-04 | DISPENSE ACK 유실 | 같은 ID 재수신에도 서보 1회 |
| AT-05 | RESULT 유실 | 같은 DISPENSE ID에 같은 XYR 재전송 |
| AT-06 | RESULT ACK 유실 | 같은 RESULT 재전송, Pi 슬롯 1회 이동 |
| AT-07 | ID 충돌 | 다른 페이로드 거부, 모터 미동작 |
| AT-08 | 좌표 불일치 | COORD_MISMATCH, 서보 미동작 |
| AT-09 | MOVING 중 리셋 | RECOVERY_REQUIRED, 자동 재이동 없음 |
| AT-10 | DISPENSING 중 리셋 | RECOVERY_REQUIRED, 자동 재배출 없음 |
| AT-11 | 센서 성공 | 정확한 XY1 반환 |
| AT-12 | 센서 미감지 | 정확한 XY0 반환, 자동 재시도 없음 |
| AT-13 | 장시간 UART 입력 | 버퍼 오버런·프레임 혼합 없음 |
| AT-14 | 전원 재인가 | EEPROM 상태·CRC 정상 복구 |

## 11. 완전 예시

```text
# 예약 도달, 슬롯 1(좌표 00), 허용시간 1시간
Pi > MOVE|00000001|0|0|003600
AT < ACK|00000001|MOVE
AT < WAIT|00000001

# 사용자가 LCD의 약 배출 버튼 터치
Pi > DISPENSE|00000002|0|0
AT < ACK|00000002|DISPENSE

# 센서가 배출구 통과를 감지
AT < RESULT|00000002|001
Pi > ACK|00000002|RESULT
```

ACK 프레임이 유실됐을 때:

```text
Pi > DISPENSE|00000002|0|0
AT: EEPROM 저장, ACK 전송 유실, 서보 1회 작동, 결과 저장

# 10초 후 같은 프레임
Pi > DISPENSE|00000002|0|0
AT: 요청번호와 페이로드가 동일하므로 서보 재동작 금지
AT < RESULT|00000002|001
Pi > ACK|00000002|RESULT
```

이 동작이 구현되어야 Pi의 허용시간까지 재전송 정책과 물리적 중복 배출 방지가 동시에 성립한다.
