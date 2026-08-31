/**
 * @file    dispense.h
 * @brief   3-phase 배출 시퀀스 (Y축 서브오프셋 흔들기 + 서보 푸시 + IR 감지)
 *
 * 기반: 상위설계 §2.2 / §3.2.4 / §5 / §6, 식별자 문서 §4.5
 *
 * 시퀀스 (상위설계 §5):
 *   1차: (y-a) 이동 -> 서보 하강(180->0) -> 0도 즉시 180도 복귀 -> 복귀 완료 후 2차
 *   2차: (y)   이동 -> 서보 하강           -> 0도 즉시 180도 복귀 -> 복귀 완료 후 3차
 *   3차: (y+a) 이동 -> 서보 하강           -> 0도 즉시 180도 복귀 시작 & 5초 타이머 동시 시작
 *
 * IR 감시 (상위설계 §6): DISPENSING 진입 시 래치 clear + 인터럽트 활성화.
 *   1~3차 전 과정(모터 이동/서보 하강·상승 포함) 상시 감시.
 *   어느 순간이든 감지되면 즉시 서보 180도 복귀 + R=1 확정 + 시퀀스 종료.
 *   3차 5초 타이머 만료까지 미감지면 R=2 (슬롯 소진 추정) 확정.
 *
 * [수정] 이전 구현은 3차 푸시 후에만 IR 을 열어 1·2차 낙하를 놓쳤고
 *        (과배출 + 오보고), 조기 종료 경로가 없었다. 상위설계 §6 대로
 *        전 구간 감시 + 즉시 종료로 재구현한다.
 *
 * X축은 배출 중 움직이지 않는다 (직전 MOVE 로 이미 정렬, 서브오프셋은 Y축
 * 전용 — 설계서 §5). Dispense_Start 의 x 파라미터는 분업 가이드라인 §3.1 의
 * 얼린 시그니처를 지키기 위해 유지하되 내부적으로는 사용하지 않는다.
 *
 * 소유: Team B
 */

#ifndef DISPENSE_H
#define DISPENSE_H

#include "config.h"
#include "types.h"

/**
 * 배출 시퀀스 시작 (상위설계 §2.2 DISPENSING 진입 동작).
 * @param x 대상 슬롯 X (검증/기록용, 시퀀스에서는 미사용).
 * @param y 대상 슬롯 Y (0..SLOT_Y_COUNT-1). 범위 밖이면 즉시
 *          DPHASE_DONE / DISPENSE_RESULT_EMPTY 로 종료한다.
 * 이미 시퀀스 진행 중이면 무시한다 (설계서 §2.2 "재시작 금지").
 */
void Dispense_Start(uint8_t x, uint8_t y);

/** 슈퍼루프 tick. */
void Dispense_Task(void);

/** 시퀀스 진행 중인가 (IDLE/DONE 이 아님). */
bool Dispense_IsBusy(void);

/** 시퀀스 완료됐는가 (DPHASE_DONE). */
bool Dispense_IsComplete(void);

/**
 * 완료 후 결과.
 *   DISPENSE_RESULT_SUCCESS : 1~3차 중 IR 감지 성공 (R=1)
 *   DISPENSE_RESULT_EMPTY   : 3차 5초까지 미감지 = 슬롯 소진 추정 (R=2)
 *   DISPENSE_RESULT_NONE    : 완료 전, 또는 Dispense_Abort() 로 중단됨
 */
DispenseResult Dispense_Result(void);

/**
 * 시퀀스 강제 중단 (분업 가이드라인 §3.1 얼린 인터페이스).
 * 서보 즉시 180도 복귀 + Y축 정지 + IR 인터럽트 차단 후 DPHASE_DONE.
 * 결과는 그 시점의 확정값을 유지한다 (미확정이면 DISPENSE_RESULT_NONE).
 */
void Dispense_Abort(void);

#endif /* DISPENSE_H */
