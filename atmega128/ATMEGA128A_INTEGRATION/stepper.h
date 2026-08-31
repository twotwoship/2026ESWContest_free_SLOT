/**
 * @file    stepper.h
 * @brief   28BYJ-48 x2 (X/Y) Full Drive 스테퍼 구동
 *
 * 기반: ATmega128A_펌웨어_식별자_v3_noEEPROM.md §4 / 상위설계 §4
 *
 * PORTA 니블 분할: X = 하위 4비트, Y = 상위 4비트 (config.h §1.4).
 * 좌표(슬롯 인덱스) -> 절대 스텝 변환 테이블은 stepper.c 내부 static const 로
 * 캡슐화한다. 다른 모듈은 슬롯 인덱스로만 좌표를 다룬다.
 *
 * [수정] 분업 가이드라인 §3.1 의 얼린 인터페이스에 맞춘다:
 *          bool Stepper_MoveToSlot(uint8_t x, uint8_t y);   // 두 축 동시
 *          bool Stepper_IsBusy(void);                        // 두 축 OR
 *          void Stepper_ReleaseAll(void);
 *        이전 구현은 이를 축별(AxisId 인자)로 쪼개 FSM(Team A) 호출부와
 *        어긋났다. 축별 저수준 함수는 Team B 내부용으로 남기되 이름을
 *        Stepper_AxisBusy() 등으로 바꿔 얼린 이름과 충돌하지 않게 한다.
 *
 * 소유: Team B
 */

#ifndef STEPPER_H
#define STEPPER_H

#include "config.h"
#include "types.h"

/** 스테퍼 초기화. DDR 출력 설정, 코일 OFF, 내부 상태 리셋. */
void Stepper_Init(void);

/** 슈퍼루프 tick. STEP_INTERVAL_MS 마다 진행 중인 이동을 각 축 한 스텝 진행. */
void Stepper_Task(void);

/* ==========================================================================
 * FSM(Team A) 이 호출하는 얼린 인터페이스 (분업 가이드라인 §3.1)
 * ========================================================================== */

/**
 * 슬롯 좌표 (x, y) 로 두 축 동시 이동 시작.
 * @return 시작했으면 true. x/y 가 슬롯 범위 밖이거나 어느 한 축이라도
 *         이미 이동 중이면 false (부분 이동 없음).
 */
bool Stepper_MoveToSlot(uint8_t x, uint8_t y);

/** 두 축 중 하나라도 목표에 아직 못 닿았으면 true. */
bool Stepper_IsBusy(void);

/** 두 축 코일 전원 차단 (PORTA 전체 LOW). 수동 복구 대기용. */
void Stepper_ReleaseAll(void);

/* ==========================================================================
 * Team B 내부 저수준 인터페이스 (homing.c / dispense.c 전용)
 * ========================================================================== */

/**
 * 절대 스텝 값 기준 단일 축 이동 시작 (배출 3-phase 서브오프셋 등 슬롯 테이블
 * 밖의 위치로 이동할 때). 축별 MIN/MAX_STEPS 범위로 클램프한다.
 * @return 시작했으면 true. 이미 이동 중이면 false.
 */
bool Stepper_MoveToStepsRaw(AxisId axis, int16_t abs_steps);

/**
 * 슬롯 인덱스를 절대 스텝 값으로 변환만 한다 (이동하지 않음).
 * @return slot_index 가 유효하면 true.
 */
bool Stepper_SlotToSteps(AxisId axis, uint8_t slot_index, int16_t *out_steps);

/** 해당 축이 이동 중인가. */
bool Stepper_AxisBusy(AxisId axis);

/**
 * 저수준 단일 스텝 (homing 전용). 인터벌 타이밍은 호출자(homing.c)가
 * HOMING_STEP_INTERVAL_MS 로 직접 관리한다. Stepper_Task() 로 이동 중인
 * 축에는 호출하지 말 것 — 경합한다.
 */
void Stepper_StepRaw(AxisId axis, StepDir dir);

/** 해당 축 이동 즉시 중지 (코일 여자 상태는 유지 = 유지토크로 위치 보존). */
void Stepper_Stop(AxisId axis);

/** 홈 탐색 완료 시 현재 위치를 기준점으로 재설정 (보통 0). 이동도 중지. */
void Stepper_ZeroAt(AxisId axis, int16_t steps);

/** 해당 축 코일 전원 차단 (de-energize). */
void Stepper_Release(AxisId axis);

#endif /* STEPPER_H */
