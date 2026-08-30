#ifndef CONFIG_H
#define CONFIG_H

/* ==========================================================================
 * 1.1 시스템                                                        [Team A]
 * ========================================================================== */
#define F_CPU						16000000UL
#include <avr/io.h>
#include "types.h"

#define SYSTICK_HZ                  1000U           /* 1ms 해상도 */
#define SYSTICK_OCR0                249U            /* ★ 16MHz / 64 / 250 = 1kHz */

/* ==========================================================================
 * 1.2 슬롯 그리드 / 좌표 검증                                       [Team A]
 * ========================================================================== */

#define SLOT_X_COUNT                2
#define SLOT_Y_COUNT                5
#define COORD_X_MIN                 0
#define COORD_X_MAX                 (SLOT_X_COUNT - 1)
#define COORD_Y_MIN                 0
#define COORD_Y_MAX                 (SLOT_Y_COUNT - 1)

#define ALLOW_TIME_MIN_SEC          1UL            
#define ALLOW_TIME_MAX_SEC          999999UL        /* TTTTTT 6자리 상한 */

/* ==========================================================================
 * 1.3 UART / 프로토콜                                               [Team A]
 * ========================================================================== */

#define UART_BAUD                   9600UL
#define UART_UBRR_VALUE             ((F_CPU / (16UL * UART_BAUD)) - 1UL)

#define UART_RX_RING_SIZE           128U            /* ★ 2의 거듭제곱 (마스킹용) */
#define UART_TX_RING_SIZE           128U

#define FRAME_MAX_LEN               64U             /* 설계서 §3.1 */
#define FRAME_TERM_LF               '\n'            /* 0x0A */
#define FRAME_TERM_CR               '\r'            /* 수신 시 제거 */
#define FRAME_DELIM                 '|'
#define FRAME_MAX_FIELDS            5U              /* MOVE 기준 최다 */
#define TX_FRAME_BUF_LEN            40U             /* ★ RESULT|8자리|XYR\n */

#define REQ_ID_HEX_LEN              8U
#define REQ_ID_NONE                 0x00000000UL    /* 설계서 §3.2.1 미사용 값 */
#define REQ_ID_MIN                  0x00000001UL
#define REQ_ID_MAX                  0xFFFFFFFFUL

/* 수신 명령 토큰 */
#define TOK_MOVE                    "MOVE"
#define TOK_DISPENSE                "DISPENSE"
#define TOK_ACK                     "ACK"
#define TOK_TIMEOUT                 "TIMEOUT"
#define TOK_RST						"RST"	//강제 초기화

/* 송신 응답 토큰 */
#define TOK_WAIT                    "WAIT"
#define TOK_RESULT                  "RESULT"
#define TOK_ERROR                   "ERROR"

/* RESULT 재전송 (설계서 §2.2 AWAITING_RESULT_ACK) */
#define RESULT_RETX_INTERVAL_MS     10000UL         /* 10초 주기 */
#define RESULT_RETX_MAX_COUNT       6U              /* 누적 60초, 초과 시 무송신 IDLE */

/* ==========================================================================
 * 1.4 스테퍼 (설계서 §4)                                            [Team B]
 * ========================================================================== */

#define STEPPER_PORT                PORTA
#define STEPPER_DDR                 DDRA
#define STEPPER_X_SHIFT             0U             
#define STEPPER_Y_SHIFT             4U              
#define STEPPER_NIBBLE_MASK         0x0FU

#define STEP_SEQ_LEN                4U              
#define STEP_INTERVAL_MS            3U             

#define X_MIN_STEPS                 0
#define X_MAX_STEPS                 1420
#define Y_MIN_STEPS                 0
#define DISPENSE_SUB_OFFSET_STEPS   60              
#define Y_MAX_STEPS                 (4600 + DISPENSE_SUB_OFFSET_STEPS)

#define STEPPER_RELEASE_ON_IDLE     0               /* 0=정지 시 유지토크, 1=코일 해제 */
/* 서브오프셋(±a)은 Y축에만 건다 (설계서 §5). dispense.c 가 AXIS_Y 로 고정. */

/*
 * 좌표 -> 절대 스텝 변환 테이블은 stepper.c 내부 static const 로 둔다.
 * 스텝 단위 값이 모듈 밖으로 나가지 않게 하기 위함.
 *
 *   static const uint8_t s_full_drive_seq[STEP_SEQ_LEN] =
 *       { 0b0011, 0b0110, 0b1100, 0b1001 };   // 검증된 TEST.STEP_MOTOR 와 동일 순서
 *       // 인덱스 감소(STEP_DIR_MINUS) = 원점 방향
 *   static const int16_t s_x_step_table[SLOT_X_COUNT] = { 0, 1420 };
 *   static const int16_t s_y_step_table[SLOT_Y_COUNT] = { 0, 1200, 2300, 3440, 4600 };
 */

/* ==========================================================================
 * 1.5 홈 탐색 (설계서 §7)                                           [Team B]
 * ========================================================================== */

#define HOMING_STEP_INTERVAL_MS     5U              /* ★ 주행보다 느리게 */
#define HOMING_MAX_STEPS_X          1800U           /* ★ 최대 행정 1420 + 여유 */
#define HOMING_MAX_STEPS_Y          6000U           /* ★ 최대 행정 4660 + 여유 */
#define HOME_LEVEL_POLL_MS          10U             /* ★ 엣지 래치 보완용 폴링 주기 */

#define HOME_DIR_X                  STEP_DIR_MINUS  /* 스텝 인덱스 감소 방향 */
#define HOME_DIR_Y                  STEP_DIR_MINUS
#define HOME_TO_ORIGIN_OFFSET_X     0               /* 센서 위치 = (0,0) 전제 */
#define HOME_TO_ORIGIN_OFFSET_Y     0

/* ==========================================================================
 * 1.6 서보 (설계서 §5)                                              [Team B]
 * ========================================================================== */

#define SERVO_DDR                   DDRB           
#define SERVO_BIT                   PB5            

#define SERVO_PWM_TOP               39999U          /* ICR1, 50Hz @ prescaler 8 */
#define SERVO_PULSE_MIN_US          500U           
#define SERVO_PULSE_MAX_US          2500U           
#define SERVO_ANGLE_MIN             0U
#define SERVO_ANGLE_MAX             180U

#define SERVO_ANGLE_IDLE            180U            /* 대기 각도 */
#define SERVO_ANGLE_PUSH_START      50U             /* DISPENSE 수신 시 즉시 점프 */
#define SERVO_ANGLE_STEP_DEG        5U              /* 램프 간격 */
#define SERVO_STEP_INTERVAL_MS      500U            /* 50~0도 총 5.0초 */
#define SERVO_SETTLE_MS             150U            /* ★ 점프 후 물리 안정 대기 — §6-6 */

/* ==========================================================================
 * 1.7 IR 센서 (설계서 §6)                                           [Team B]
 * ========================================================================== */

#define IR1_PORT                    PORTD           /* 내부 풀업 설정용 */
#define IR1_DDR                     DDRD
#define IR1_BIT                     PD2             // INT2
#define IR2_PORT                    PORTE
#define IR2_DDR                     DDRE
#define IR2_BIT                     PE4             // INT4

/* IR 은 falling-edge 하드웨어 인터럽트로만 감지한다 (설계서 §6). 감지 = LOW
 * 라는 극성은 EICRB/EICRA 의 falling-edge 설정에 이미 반영되어 있어 런타임
 * 레벨 판독은 하지 않는다 (PINx 매크로 불필요). */
#define IR_INTERNAL_PULLUP          1U              /* NPN 오픈 컬렉터 -> 내부 풀업 ON */
/* IR_DEBOUNCE_MS 제거: 설계서 §6 은 순수 falling-edge 즉시 감지. 서보-센서가
 * 물리적으로 떨어져 있어 진동 오검출 우려가 없고, 재확인 디바운스는 오히려
 * 빔을 3~8ms 만에 통과하는 빠른 낙하를 스파이크로 오인해 놓친다.
 * 전기적 노이즈는 설계서 §7 상 처리 범위 밖. */

#define DISPENSE_DETECT_WINDOW_MS   5000UL          /* 3차 5초 감지창 */

/* ==========================================================================
 * 1.8 홈 센서 (설계서 §7)                                       
 * ========================================================================== */

#define HOME_X_PORT                 PORTD           
#define HOME_X_DDR                  DDRD           
#define HOME_X_PIN_REG              PIND
#define HOME_X_BIT                  PD0             // INT0

#define HOME_Y_PORT                 PORTD           
#define HOME_Y_DDR                  DDRD            
#define HOME_Y_PIN_REG              PIND
#define HOME_Y_BIT                  PD1             // INT1

#define HOME_ACTIVE_LEVEL           1U              /* 빔 차단(감지) = HIGH */
#define HOME_INTERNAL_PULLUP        0U              /* 외부 풀업 4.7k -> 내부 OFF */

#endif /* CONFIG_H */