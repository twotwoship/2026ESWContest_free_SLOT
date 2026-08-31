/**
 * @file    stepper.c
 * @brief   28BYJ-48 x2 (X/Y) Full Drive 스테퍼 구동 구현
 *
 * PORTA 니블 분할 (config.h §1.4)
 *   X : 비트 0~3 (STEPPER_X_SHIFT = 0)
 *   Y : 비트 4~7 (STEPPER_Y_SHIFT = 4)
 *
 * Full Drive 4-step 시퀀스: AB -> BC -> CD -> DA (2상 여자).
 * STEP_DIR_PLUS 는 시퀀스 인덱스 증가, STEP_DIR_MINUS 는 감소 방향이다.
 *
 * 비트 조작은 _BV() 대신 (1 << n) 을 직접 쓴다. 8비트 레지스터 대입에는
 * (uint8_t) 캐스팅을 붙인다.
 */

#include <avr/io.h>

#include "stepper.h"
#include "systick.h"

/* ==========================================================================
 * 내부 상수 테이블
 * ========================================================================== */

/*
 * [수정] 검증된 TEST.STEP_MOTOR/step_motor.c 의 FULL_STEP 테이블과 동일한
 *        순서로 맞춘다. 이전 순서({1100,0110,0011,1001})는 인덱스 0↔2 가
 *        뒤바뀌어 있어 STEP_DIR 부호와 물리 회전 방향의 대응이 통째로
 *        반전됐다 — 그 결과 HOME_DIR(STEP_DIR_MINUS)이 원점 반대 방향으로
 *        구동되어 전원 인가 시 홈이 아닌 쪽으로 회전했다.
 *        TEST 코드 기준: 인덱스 감소(=STEP_DIR_MINUS) 방향이 원점 방향.
 */
static const uint8_t s_full_drive_seq[STEP_SEQ_LEN] =
    { 0x03U /* 0b0011 */, 0x06U /* 0b0110 */, 0x0CU /* 0b1100 */, 0x09U /* 0b1001 */ };

static const int16_t s_x_step_table[SLOT_X_COUNT] = { 0, 1420 };
static const int16_t s_y_step_table[SLOT_Y_COUNT] = { 0, 1200, 2300, 3440, 4600 };

/* ==========================================================================
 * 내부 상태
 * ========================================================================== */

typedef struct {
    int16_t        cur_steps;
    int16_t        target_steps;
    uint8_t        seq_index;
    StepperStatus  status;
    uint32_t       last_tick_ms;
} StepperAxis;

static StepperAxis s_axis[AXIS_COUNT];

/* ==========================================================================
 * 내부 헬퍼
 * ========================================================================== */

/* 해당 축 니블에만 4비트 패턴을 씀. 다른 축 니블은 보존 */
static void Stepper_WritePhase(AxisId axis, uint8_t pattern)
{
    uint8_t shift = (axis == AXIS_X) ? STEPPER_X_SHIFT : STEPPER_Y_SHIFT;
    uint8_t mask  = (uint8_t)(STEPPER_NIBBLE_MASK << shift);

    STEPPER_PORT = (uint8_t)((STEPPER_PORT & (uint8_t)~mask)
                    | (uint8_t)((pattern & STEPPER_NIBBLE_MASK) << shift));
}

/* 축별 절대 스텝 하한/상한 */
static void Stepper_AxisBounds(AxisId axis, int16_t *out_min, int16_t *out_max)
{
    if (axis == AXIS_X) {
        *out_min = X_MIN_STEPS;
        *out_max = X_MAX_STEPS;
    } else {
        *out_min = Y_MIN_STEPS;
        *out_max = Y_MAX_STEPS;
    }
}

/* 갑 클램핑 */
static int16_t Stepper_Clamp(int16_t v, int16_t lo, int16_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* seq_index 를 dir 방향으로 한 칸 돌리고 그 패턴을 출력 */
static void Stepper_AdvancePhase(AxisId axis, StepDir dir)
{
    StepperAxis *ax = &s_axis[axis];

    if (dir == STEP_DIR_PLUS) {
        ax->seq_index = (uint8_t)((ax->seq_index + 1U) % STEP_SEQ_LEN);
    } else {
        ax->seq_index = (uint8_t)((ax->seq_index + STEP_SEQ_LEN - 1U) % STEP_SEQ_LEN);
    }

    Stepper_WritePhase(axis, s_full_drive_seq[ax->seq_index]);
    ax->cur_steps = (int16_t)(ax->cur_steps + (int16_t)dir);
}

/* ==========================================================================
 * 초기화
 * ========================================================================== */

void Stepper_Init(void)
{
    uint8_t i;

    STEPPER_DDR  = 0xFFU;   /* 8비트 전부 출력 (X 니블 + Y 니블) */
    STEPPER_PORT = 0x00U;   /* 코일 전부 OFF 로 시작 */

    for (i = 0U; i < (uint8_t)AXIS_COUNT; i++) {
        s_axis[i].cur_steps    = 0;
        s_axis[i].target_steps = 0;
        s_axis[i].seq_index    = 0U;
        s_axis[i].status       = STEPPER_IDLE;
        s_axis[i].last_tick_ms = 0UL;
    }
}

/* ==========================================================================
 * 슈퍼루프 tick
 * ========================================================================== */

void Stepper_Task(void)
{
    uint8_t  i;
    uint32_t now = Systick_Now();

    for (i = 0U; i < (uint8_t)AXIS_COUNT; i++) {
        StepperAxis *ax = &s_axis[i];
        StepDir      dir;

        if (ax->status != STEPPER_RUNNING) {
            continue;
        }

        if (!Systick_Elapsed(ax->last_tick_ms, STEP_INTERVAL_MS)) {
            continue;
        }
        ax->last_tick_ms = now;

        if (ax->cur_steps == ax->target_steps) {
            ax->status = STEPPER_IDLE;
#if (STEPPER_RELEASE_ON_IDLE != 0)
            Stepper_Release((AxisId)i);
#endif
            continue;
        }

        dir = (ax->target_steps > ax->cur_steps) ? STEP_DIR_PLUS : STEP_DIR_MINUS;
        Stepper_AdvancePhase((AxisId)i, dir);
    }
}

/* ==========================================================================
 * 얼린 인터페이스 (FSM 호출)
 * ========================================================================== */

bool Stepper_MoveToSlot(uint8_t x, uint8_t y)
{
    int16_t xs, ys;

    if (!Stepper_SlotToSteps(AXIS_X, x, &xs)) {
        return false;
    }
    if (!Stepper_SlotToSteps(AXIS_Y, y, &ys)) {
        return false;
    }
    if (Stepper_AxisBusy(AXIS_X) || Stepper_AxisBusy(AXIS_Y)) {
        return false;   /* 부분 이동을 만들지 않는다 */
    }

    (void)Stepper_MoveToStepsRaw(AXIS_X, xs);
    (void)Stepper_MoveToStepsRaw(AXIS_Y, ys);
    return true;
}

bool Stepper_IsBusy(void)
{
    return (s_axis[AXIS_X].status == STEPPER_RUNNING)
        || (s_axis[AXIS_Y].status == STEPPER_RUNNING);
}

void Stepper_ReleaseAll(void)
{
    STEPPER_PORT = 0x00U;
    s_axis[AXIS_X].status = STEPPER_IDLE;
    s_axis[AXIS_Y].status = STEPPER_IDLE;
}

/* ==========================================================================
 * 내부 저수준 인터페이스 (homing.c / dispense.c 전용)
 * ========================================================================== */

bool Stepper_SlotToSteps(AxisId axis, uint8_t slot_index, int16_t *out_steps)
{
    if (axis == AXIS_X) {
        if (slot_index >= SLOT_X_COUNT) {
            return false;
        }
        *out_steps = s_x_step_table[slot_index];
        return true;
    } else if (axis == AXIS_Y) {
        if (slot_index >= SLOT_Y_COUNT) {
            return false;
        }
        *out_steps = s_y_step_table[slot_index];
        return true;
    }

    return false;
}

bool Stepper_MoveToStepsRaw(AxisId axis, int16_t abs_steps)
{
    int16_t lo, hi, clamped;
    StepperAxis *ax;

    if ((uint8_t)axis >= (uint8_t)AXIS_COUNT) {
        return false;
    }

    ax = &s_axis[axis];
    if (ax->status == STEPPER_RUNNING) {
        return false;
    }

    Stepper_AxisBounds(axis, &lo, &hi);
    clamped = Stepper_Clamp(abs_steps, lo, hi);

    ax->target_steps = clamped;

    if (ax->cur_steps == clamped) {
        ax->status = STEPPER_IDLE;
        return true;
    }

    ax->status       = STEPPER_RUNNING;
    ax->last_tick_ms  = Systick_Now();
    return true;
}

bool Stepper_AxisBusy(AxisId axis)
{
    if ((uint8_t)axis >= (uint8_t)AXIS_COUNT) {
        return false;
    }
    return (s_axis[axis].status == STEPPER_RUNNING);
}

void Stepper_StepRaw(AxisId axis, StepDir dir)
{
    if ((uint8_t)axis >= (uint8_t)AXIS_COUNT) {
        return;
    }
    if (dir == STEP_DIR_NONE) {
        return;
    }
    Stepper_AdvancePhase(axis, dir);
}

void Stepper_Stop(AxisId axis)
{
    StepperAxis *ax;

    if ((uint8_t)axis >= (uint8_t)AXIS_COUNT) {
        return;
    }

    ax = &s_axis[axis];
    ax->status       = STEPPER_IDLE;
    ax->target_steps = ax->cur_steps;
    /* 코일 패턴은 그대로 둔다 = 유지토크로 위치 보존 */
}

void Stepper_ZeroAt(AxisId axis, int16_t steps)
{
    StepperAxis *ax;

    if ((uint8_t)axis >= (uint8_t)AXIS_COUNT) {
        return;
    }

    ax = &s_axis[axis];
    ax->cur_steps    = steps;
    ax->target_steps = steps;
    ax->status       = STEPPER_IDLE;
}

void Stepper_Release(AxisId axis)
{
    if ((uint8_t)axis >= (uint8_t)AXIS_COUNT) {
        return;
    }
    Stepper_WritePhase(axis, 0x00U);
    s_axis[axis].status = STEPPER_IDLE;
}
