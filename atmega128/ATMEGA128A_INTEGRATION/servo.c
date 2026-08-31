/**
 * @file    servo.c
 * @brief   배출 푸시 서보 구현 (상위설계 §5)
 *
 * Timer1 Fast PWM Mode 14 (WGM13:0 = 1110, TOP = ICR1), 50Hz.
 * 16MHz / prescaler 8 = 2MHz 타이머 클럭 -> 1 tick = 0.5us.
 * SERVO_BIT(PB5) = OC1A 핀.
 *
 * 내부 스테이지 (s_stage, 외부 비노출):
 *   SV_IDLE      : 정지. 180도 대기.
 *   SV_RAMP      : 50->0 하강 (5도 / SERVO_STEP_INTERVAL_MS). 진입 시 180->50 점프.
 *   SV_HOLD_ZERO : 램프 완료. 0도 유지. 호출자의 Servo_ReturnIdle() 대기.
 *   SV_SETTLE_UP : ->180 점프 직후 물리 안정 대기 (SERVO_SETTLE_MS).
 *
 * Servo_IsBusy() 는 RAMP / SETTLE_UP 에서 true, IDLE / HOLD_ZERO 에서 false.
 * IDLE(180 대기) 와 HOLD_ZERO(0도 홀드) 의 구분은 호출자(dispense)가 한다.
 *
 * 비트 조작은 _BV() 대신 (1 << n) 을 직접 쓴다.
 */

#include <avr/io.h>

#include "servo.h"
#include "systick.h"

/* ==========================================================================
 * 내부 상태
 * ========================================================================== */

#define SV_IDLE        0U
#define SV_RAMP        1U
#define SV_HOLD_ZERO   2U
#define SV_SETTLE_UP   3U

static uint8_t  s_stage;
static uint8_t  s_cur_angle;
static uint32_t s_last_ms;

/* ==========================================================================
 * 내부 헬퍼
 * ========================================================================== */

/** 각도를 OCR1A 펄스폭으로 변환해서 즉시 반영 */
static void Servo_WriteAngle(uint8_t angle)
{
    uint32_t pulse_us = SERVO_PULSE_MIN_US
        + (((uint32_t)(SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US) * angle) / SERVO_ANGLE_MAX);

    /* prescaler 8 -> 1 tick = 0.5us -> ticks = us * 2 */
    OCR1A = (uint16_t)(pulse_us * 2UL);
}

/* ==========================================================================
 * 초기화
 * ========================================================================== */

void Servo_Init(void)
{
    SERVO_DDR |= (uint8_t)(1U << SERVO_BIT);

    /* Fast PWM Mode 14: WGM13=1,WGM12=1,WGM11=1,WGM10=0, TOP=ICR1.
     * COM1A1=1,COM1A0=0 -> OC1A non-inverting.
     * CS12:10 = 010 (prescaler 8) -> CS11 만 세트. */
    TCCR1A = (uint8_t)((1U << COM1A1) | (1U << WGM11));
    TCCR1B = (uint8_t)((1U << WGM13) | (1U << WGM12) | (1U << CS11));
    ICR1   = SERVO_PWM_TOP;

    s_stage     = SV_IDLE;
    s_cur_angle = SERVO_ANGLE_IDLE;
    s_last_ms   = 0UL;

    Servo_WriteAngle(s_cur_angle);
}

/* ==========================================================================
 * 공개 인터페이스
 * ========================================================================== */

void Servo_StartPushRamp(void)
{
    if (s_stage != SV_IDLE) {
        return;   /* 이미 램프/복귀 진행 중이면 무시 */
    }

    /* 상위설계 §5: 50도로 즉시 점프, 이후 500ms 간격으로 5도씩 감소. */
    s_cur_angle = SERVO_ANGLE_PUSH_START;
    Servo_WriteAngle(s_cur_angle);

    s_stage   = SV_RAMP;
    s_last_ms = Systick_Now();
}

void Servo_ReturnIdle(void)
{
    /* 어느 스테이지에서든(램프 도중 IR 감지, 0도 홀드, 이미 대기 등) 즉시
     * 180도로 점프한 뒤 안정대기. 상위설계 §2.2 "즉시 서보 180도 복귀". */
    s_cur_angle = SERVO_ANGLE_IDLE;
    Servo_WriteAngle(s_cur_angle);

    s_stage   = SV_SETTLE_UP;
    s_last_ms = Systick_Now();
}

bool Servo_IsBusy(void)
{
    return (s_stage == SV_RAMP) || (s_stage == SV_SETTLE_UP);
}

/* ==========================================================================
 * 슈퍼루프 tick
 * ========================================================================== */

void Servo_Task(void)
{
    switch (s_stage) {

    case SV_RAMP:   /* 50->0, 5도 / SERVO_STEP_INTERVAL_MS */
        if (Systick_Elapsed(s_last_ms, SERVO_STEP_INTERVAL_MS)) {
            s_last_ms = Systick_Now();

            if (s_cur_angle <= SERVO_ANGLE_STEP_DEG) {
                s_cur_angle = SERVO_ANGLE_MIN;
            } else {
                s_cur_angle = (uint8_t)(s_cur_angle - SERVO_ANGLE_STEP_DEG);
            }
            Servo_WriteAngle(s_cur_angle);

            if (s_cur_angle == SERVO_ANGLE_MIN) {
                /* 0도 도달. 복귀는 호출자가 지시한다 (1·2차는 복귀 완료 대기,
                 * 3차는 복귀와 5초 감지창을 병행). */
                s_stage = SV_HOLD_ZERO;
            }
        }
        break;

    case SV_SETTLE_UP:   /* ->180 점프 직후 안정 -> 사이클 종료 */
        if (Systick_Elapsed(s_last_ms, SERVO_SETTLE_MS)) {
            s_stage = SV_IDLE;
        }
        break;

    case SV_IDLE:
    case SV_HOLD_ZERO:
    default:
        break;
    }
}
