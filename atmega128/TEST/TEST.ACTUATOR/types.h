/**
 * @file    types.h
 * @brief   BLSlot(약속) ATmega128A 공용 enum 정의
 *
 * 기반: ATmega128A_펌웨어_식별자_v3_noEEPROM.md §2
 *
 * 표기 규칙: 상수 UPPER_SNAKE, 타입 PascalCase,
 *            함수 Module_PascalCase, 전역 g_snake_case, 정적 s_snake_case
 *
 * [수정] StepperAxis / ServoCtx / SensorCtx / HomingCtx / DispenseCtx 는
 *        여기 두지 않는다. 함수 시그니처에 노출되지 않는 내부 상태이므로
 *        각 소유 .c 파일 상단에 static 타입으로 정의한다 (캡슐화 원칙).
 *        여기 남는 건 반환형/파라미터로 모듈 경계를 넘는 enum 뿐이다.
 *        ServoStatus 는 공개 함수가 없어져 제거했다 (Servo_IsBusy() 만 노출).
 *
 * 소유권 (팀 분업 가이드라인 §2.1):
 *   Team A : SystemState, CmdType, ErrorCode, Frame, LastCmdRecord, SystemCtx
 *   Team B : AxisId, StepDir, DispenseResult, DispensePhase, StepperStatus,
 *            HomingStatus
 *
 * 주의: 이 파일은 config.h 를 포함하지 않는다 (config.h 가 이 파일을 포함).
 *       순환 포함을 막기 위해 여기에는 config.h 의 매크로를 쓰지 않는다.
 */

#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stdbool.h>

/* ==========================================================================
 * enum
 * ========================================================================== */

/** 시스템 상태 (설계서 §2, flat 6-state) */
typedef enum {
	STATE_IDLE = 0,
	STATE_MOVING,
	STATE_AWAITING_DISPENSE,
	STATE_DISPENSING,
	STATE_AWAITING_RESULT_ACK,
	STATE_RECOVERY_REQUIRED
} SystemState;


/** X/Y축 식별. AXIS_COUNT 는 배열 크기용 */
typedef enum {
	AXIS_X = 0,
	AXIS_Y,
	AXIS_COUNT
} AxisId;

/** 스테퍼 이동 방향. 스텝 인덱스 증감 부호와 일치 */
typedef enum {
	STEP_DIR_MINUS = -1,
	STEP_DIR_NONE  = 0,
	STEP_DIR_PLUS  = 1
} StepDir;


/** 배출 결과. 값은 RESULT 프레임의 R 필드와 동일 */
typedef enum {
	DISPENSE_RESULT_NONE    = 0,
	DISPENSE_RESULT_SUCCESS = 1,
	DISPENSE_RESULT_EMPTY   = 2
} DispenseResult;


/**
 * 배출 시퀀스 서브상태 (dispense.c 전용).
 * FSM 은 이 값을 보지 않고 Dispense_IsBusy() / Dispense_IsComplete() 만 쓴다.
 *
 * [수정] 1~3차가 "이동 -> 하강 -> 복귀" 로 동일 구조라, 차수별 phase 를
 *        따로 두지 않고 (MOVE/PUSH/RETURN) 서브상태 + 차수 카운터(dispense.c
 *        내부 s_ctx.round) 로 표현한다. 3차만 RETURN 대신 WINDOW 로 분기
 *        (0도 도달 즉시 복귀 + 5초 감지창 병행, 상위설계 §2.2 / §5).
 */
typedef enum {
	DPHASE_IDLE = 0,
	DPHASE_MOVE,      /**< 현재 차수 목표 스텝으로 Y 이동 중 */
	DPHASE_PUSH,      /**< 서보 50->0 램프 중 */
	DPHASE_RETURN,    /**< 서보 180도 복귀 완료 대기 (1·2차) */
	DPHASE_WINDOW,    /**< 3차: 5초 IR 감지창 (서보 복귀와 병행) */
	DPHASE_DONE
} DispensePhase;


/** 스테퍼 상태 (stepper.c 내부, Stepper_IsBusy() 로만 공개) */
typedef enum {
	STEPPER_IDLE = 0,
	STEPPER_RUNNING
} StepperStatus;


/* 서보 상태: servo.c 내부 s_stage 로만 관리하고 Servo_IsBusy() 로만 공개한다.
 * 외부로 넘어가는 enum 이 없으므로 여기 두지 않는다. */


/** 홈 탐색 상태 (설계서 §7) */
typedef enum {
	HOMING_IDLE = 0,
	HOMING_SEARCHING,    /**< 자동 복구: 스텝 예산 내 탐색 */
	HOMING_MANUAL_WAIT,  /**< 수동 복구: 코일 해제, 사람이 밀기 대기 */
	HOMING_DONE
} HomingStatus;

#endif /* TYPES_H */
