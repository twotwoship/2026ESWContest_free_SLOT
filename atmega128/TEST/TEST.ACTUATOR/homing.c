/**
 * @file    homing.c
 * @brief   전원 인가 시 무조건 X/Y 동시 홈 탐색 구현 (상위설계 §2.1 / §7)
 *
 * 두 축을 동시에 홈 방향으로 HOMING_STEP_INTERVAL_MS 간격으로 한 스텝씩
 * 구동한다. 각 축은 자신의 Sensors_HomeLatched()/Sensors_HomeLevel() 이
 * 서면 즉시 완료 처리(스텝 원점 재설정 + 코일 OFF)하고, 나머지 축은 계속
 * 진행한다 — 엣지 인터럽트든 Sensors_Task() 의 레벨 폴링(이미 원점 위에서
 * 부팅한 경우 보완)이든 소스는 상관하지 않는다.
 *
 * 어느 한 축이라도 스텝 예산을 초과하면 두 축 모두 정지 + 코일 해제 후
 * HOMING_MANUAL_WAIT. 이때 두 축의 완료 플래그를 모두 초기화하여, 사람이
 * 두 축을 모두 원점으로 밀어 두 센서가 다시 감지되어야 (0,0) 을 확정한다
 * (상위설계 §2.1 "두 축의 센서를 모두 감지 시").
 */

#include "homing.h"
#include "stepper.h"
#include "sensors.h"
#include "systick.h"

/* ==========================================================================
 * 내부 상태
 * ========================================================================== */

typedef struct {
    HomingStatus status;
    bool         x_done;
    bool         y_done;
    uint16_t     x_step_count;
    uint16_t     y_step_count;
    uint32_t     last_tick_ms;
} HomingCtx;

static HomingCtx s_ctx;

/* ==========================================================================
 * 내부 헬퍼
 * ========================================================================== */

/** 해당 축이 지금 원점 위에 있는가 (엣지 래치 또는 현재 레벨). */
static bool Homing_AxisAtHome(AxisId axis)
{
    return Sensors_HomeLatched(axis) || Sensors_HomeLevel(axis);
}

/** 해당 축 완료 처리: 스텝 원점 재설정 + 코일 OFF (상위설계 §7). */
static void Homing_MarkAxisDone(AxisId axis)
{
    int16_t origin = (axis == AXIS_X) ? HOME_TO_ORIGIN_OFFSET_X
                                      : HOME_TO_ORIGIN_OFFSET_Y;
    Stepper_ZeroAt(axis, origin);   /* 센서 트립 지점과 (0,0) 의 보정. 기본 0 */
    Stepper_Release(axis);
    if (axis == AXIS_X) {
        s_ctx.x_done = true;
    } else {
        s_ctx.y_done = true;
    }
}

/**
 * 스텝 예산 초과: 두 축 모두 정지 + 코일 해제, 완료 플래그 리셋 후 수동 대기
 * (상위설계 §2.1).
 */
static void Homing_EnterManualWait(void)
{
    s_ctx.status = HOMING_MANUAL_WAIT;
    s_ctx.x_done = false;
    s_ctx.y_done = false;
    Stepper_ReleaseAll();
    Sensors_HomeClear(AXIS_X);
    Sensors_HomeClear(AXIS_Y);
}

/* ==========================================================================
 * 공개 인터페이스
 * ========================================================================== */

void Homing_Start(void)
{
    s_ctx.status       = HOMING_SEARCHING;
    s_ctx.x_done       = false;
    s_ctx.y_done       = false;
    s_ctx.x_step_count = 0U;
    s_ctx.y_step_count = 0U;
    s_ctx.last_tick_ms = Systick_Now();

    Sensors_HomeClear(AXIS_X);
    Sensors_HomeClear(AXIS_Y);

    /* 이미 원점에 정지한 채 부팅한 경우: 엣지가 안 생기므로 레벨로 판정. */
    if (Sensors_HomeLevel(AXIS_X)) {
        Homing_MarkAxisDone(AXIS_X);
    }
    if (Sensors_HomeLevel(AXIS_Y)) {
        Homing_MarkAxisDone(AXIS_Y);
    }
    if (s_ctx.x_done && s_ctx.y_done) {
        s_ctx.status = HOMING_DONE;
    }
}

bool Homing_IsComplete(void)
{
    return (s_ctx.status == HOMING_DONE);
}

HomingStatus Homing_Status(void)
{
    return s_ctx.status;
}

void Homing_Task(void)
{
    uint32_t now;

    if (s_ctx.status == HOMING_DONE) {
        return;
    }

    /* 상태(SEARCHING/MANUAL_WAIT) 무관하게 매 tick 두 축 감지부터 확인한다.
     * MANUAL_WAIT 중에는 이게 유일한 탈출 경로다. */
    if (!s_ctx.x_done && Homing_AxisAtHome(AXIS_X)) {
        Homing_MarkAxisDone(AXIS_X);
    }
    if (!s_ctx.y_done && Homing_AxisAtHome(AXIS_Y)) {
        Homing_MarkAxisDone(AXIS_Y);
    }

    if (s_ctx.x_done && s_ctx.y_done) {
        s_ctx.status = HOMING_DONE;
        return;
    }

    if (s_ctx.status != HOMING_SEARCHING) {
        return;   /* HOMING_MANUAL_WAIT: 스텝 구동 없이 감지만 대기 */
    }

    now = Systick_Now();
    if (!Systick_Elapsed(s_ctx.last_tick_ms, HOMING_STEP_INTERVAL_MS)) {
        return;
    }
    s_ctx.last_tick_ms = now;

    /* 상위설계 §7: 미완료 축을 동시에 홈 방향으로 한 스텝씩. */
    if (!s_ctx.x_done) {
        Stepper_StepRaw(AXIS_X, HOME_DIR_X);
        if (++s_ctx.x_step_count >= HOMING_MAX_STEPS_X) {
            Homing_EnterManualWait();
            return;
        }
    }
    if (!s_ctx.y_done) {
        Stepper_StepRaw(AXIS_Y, HOME_DIR_Y);
        if (++s_ctx.y_step_count >= HOMING_MAX_STEPS_Y) {
            Homing_EnterManualWait();
            return;
        }
    }
}
