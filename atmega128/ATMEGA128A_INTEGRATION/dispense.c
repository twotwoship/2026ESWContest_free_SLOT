/**
 * @file    dispense.c
 * @brief   3-회 배출 시퀀스 구현 (상위설계 §2.2 / §3.2.4 / §5 / §6)
 *
 * 1~3차가 모두 동일 구조라 차수별 상태를 나열하지 않고 서브상태 4개
 * (MOVE / PUSH / RETURN / WINDOW) + 차수 카운터 s_ctx.round 로 표현한다.
 *
 *   MOVE   : 이번 차수 목표 스텝(center + offset[round])으로 Y 이동 대기
 *   PUSH   : Servo_StartPushRamp() -> Servo_IsBusy() 풀림 = 0도 도달
 *   RETURN : Servo_ReturnIdle() -> 복귀 완료 대기 (1·2차). 완료 시 다음 차수 MOVE
 *   WINDOW : 3차 전용. 0도 도달 즉시 복귀 지시 + 5초 IR 감지창 (복귀와 병행)
 *
 * IR 상시 감시 (상위설계 §6): 매 tick, 시퀀스 진행 중이면 전 구간에서
 * falling edge 확인. 잡히면 Dispense_End(SUCCESS) — 즉시 종료.
 * 순수 엣지 기반 (서보-센서 이격, 재확인 창은 빠른 낙하를 놓침).
 */

#include "dispense.h"
#include "stepper.h"
#include "servo.h"
#include "sensors.h"
#include "systick.h"

/* ==========================================================================
 * 내부 상태
 * ========================================================================== */

#define DISPENSE_ROUND_COUNT  3U

/* 차수별 Y 서브오프셋: 1차 우(-a) -> 2차 중(0) -> 3차 좌(+a). 상위설계 §5 */
static const int16_t s_round_offset[DISPENSE_ROUND_COUNT] = {
    -(int16_t)DISPENSE_SUB_OFFSET_STEPS,
    0,
    +(int16_t)DISPENSE_SUB_OFFSET_STEPS
};

typedef struct {
    DispensePhase  phase;
    uint8_t        round;            /* 0..DISPENSE_ROUND_COUNT-1 */
    int16_t        center_steps;     /* Y_STEP_TABLE[target_y] */
    DispenseResult result;
    uint32_t       window_start_ms;
} DispenseCtx;

static DispenseCtx s_ctx;

/* ==========================================================================
 * 내부 헬퍼
 * ========================================================================== */

/**
 * 시퀀스 종료 (공통: IR 성공 / 5초 만료 / 강제 중단).
 * IR 차단 + 서보 즉시 180도 복귀 + Y축 정지 + DPHASE_DONE.
 * @param r 확정 결과. DISPENSE_RESULT_NONE 이면 결과값을 건드리지 않는다(Abort).
 */
static void Dispense_End(DispenseResult r)
{
    if (r != DISPENSE_RESULT_NONE) {
        s_ctx.result = r;
    }
    Sensors_IrEnable(false);
    Servo_ReturnIdle();
    Stepper_Stop(AXIS_Y);
    s_ctx.phase = DPHASE_DONE;
}

/** 현재 차수 목표 스텝으로 Y 이동 시작 + DPHASE_MOVE 진입. */
static void Dispense_BeginRound(void)
{
    int16_t target = (int16_t)(s_ctx.center_steps + s_round_offset[s_ctx.round]);
    s_ctx.phase = DPHASE_MOVE;
    (void)Stepper_MoveToStepsRaw(AXIS_Y, target);   /* MIN/MAX 클램프는 내부에서 */
}

/* ==========================================================================
 * 공개 인터페이스
 * ========================================================================== */

void Dispense_Start(uint8_t x, uint8_t y)
{
    int16_t center;

    (void)x;   /* X축은 배출 중 미사용 (dispense.h 참고) */

    if (Dispense_IsBusy()) {
        return;   /* 상위설계 §2.2: 같은 DISPENSE 재수신 시 재시작 금지 */
    }

    if (!Stepper_SlotToSteps(AXIS_Y, y, &center)) {
        /* 범위 밖 좌표. FSM 에서 미리 걸러져야 정상이지만 방어적으로 실패 종료. */
        s_ctx.result = DISPENSE_RESULT_EMPTY;
        s_ctx.phase  = DPHASE_DONE;
        return;
    }

    s_ctx.center_steps    = center;
    s_ctx.round           = 0U;
    s_ctx.result          = DISPENSE_RESULT_NONE;
    s_ctx.window_start_ms = 0UL;

    /* 상위설계 §6: DISPENSING 진입 시 래치 clear + 전 과정 인터럽트 활성화 */
    Sensors_IrClear();
    Sensors_IrEnable(true);

    Dispense_BeginRound();
}

// 진행 중 여부
bool Dispense_IsBusy(void)
{
    return (s_ctx.phase != DPHASE_IDLE) && (s_ctx.phase != DPHASE_DONE);
}

// DPHASE_DONE 도달 여부
bool Dispense_IsComplete(void)
{
    return (s_ctx.phase == DPHASE_DONE);
}

// 확정된 R 값 회수
DispenseResult Dispense_Result(void)
{
    return s_ctx.result;
}

// 즉시 서보 180도 복귀
void Dispense_Abort(void)
{
    Dispense_End(DISPENSE_RESULT_NONE);   /* result 는 현재 확정값 유지 */
}

/* ==========================================================================
 * 슈퍼루프 tick
 * ========================================================================== */

void Dispense_Task(void)
{
    /* 상위설계 §6: 시퀀스 진행 중이면 전 구간에서 IR 상시 감시 */
    if (Dispense_IsBusy() && Sensors_IrDetected()) {
        Dispense_End(DISPENSE_RESULT_SUCCESS);
        return;
    }

    switch (s_ctx.phase) {

    case DPHASE_MOVE:
        if (!Stepper_AxisBusy(AXIS_Y)) {
            Servo_StartPushRamp();
            s_ctx.phase = DPHASE_PUSH;
        }
        break;

    case DPHASE_PUSH:
        if (!Servo_IsBusy()) {                 /* 램프 0도 도달 (홀드) */
            Servo_ReturnIdle();                /* 상위설계 §5: 0도 도달 즉시 복귀 */
            if (s_ctx.round == (DISPENSE_ROUND_COUNT - 1U)) {
                /* 3차: 복귀 완료를 기다리지 않고 5초 감지창을 병행 시작 */
                s_ctx.window_start_ms = Systick_Now();
                s_ctx.phase = DPHASE_WINDOW;
            } else {
                s_ctx.phase = DPHASE_RETURN;
            }
        }
        break;

    case DPHASE_RETURN:
        if (!Servo_IsBusy()) {                 /* 180도 복귀 완료 -> 다음 차수 */
            s_ctx.round++;
            Dispense_BeginRound();
        }
        break;

    case DPHASE_WINDOW:
        /* 감지 성공은 상단 공통 블록에서 처리. 여기서는 만료 시 R=2 확정. */
        if (Systick_Elapsed(s_ctx.window_start_ms, DISPENSE_DETECT_WINDOW_MS)) {
            Dispense_End(DISPENSE_RESULT_EMPTY);
        }
        break;

    case DPHASE_IDLE:
    case DPHASE_DONE:
    default:
        break;
    }
}
