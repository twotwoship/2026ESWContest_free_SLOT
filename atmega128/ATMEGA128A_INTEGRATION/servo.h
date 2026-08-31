/**
 * @file    servo.h
 * @brief   배출 푸시 서보 (Timer1 Fast PWM, 50Hz)
 *
 * 기반: ATmega128A_펌웨어_식별자_v3_noEEPROM.md §5, §4.7 / 상위설계 §5
 *
 * 확정된 동작 프로파일 (상위설계 §5 / config.h §1.6):
 *   180도(대기) -> 50도 : 즉시 점프 (DISPENSE 램프 시작점)
 *   50도 -> 0도          : 5도 / 500ms 램프 (실제 푸시 동작, 총 5.0초)
 *   0도 -> 180도(복귀)   : 즉시 점프 + SERVO_SETTLE_MS 물리 안정
 *
 * [수정] 상위설계 §5 재정합. 이전 구현은 Servo_PushStart() 한 번으로 램프와
 *        복귀를 통째로 자동 실행했으나, 상위설계는 차수마다 다음이 필요하다:
 *          - 1·2차: "0도 도달 즉시 180도 복귀 -> 복귀 완료 후 다음 차수 이동"
 *          - 3차:   "0도 도달 즉시 180도 복귀 시작 & 5초 타이머 동시 시작"
 *        즉 dispense.c 가 "램프 완료(0도 도달)" 시점을 잡아 스스로 복귀를
 *        지시해야 한다. 그래서 식별자 문서 §4.7 대로 램프와 복귀를 분리한다:
 *          Servo_StartPushRamp() : 180->50 점프 후 50->0 램프. 0도에서 정지(홀드).
 *          Servo_ReturnIdle()    : 현재 각도가 무엇이든 즉시 180도로 복귀.
 *        Servo_IsBusy() 는 램프/복귀 진행 중이면 true, 0도 홀드/180도 대기면
 *        false. 호출자는 dispense phase 로 둘을 구분한다.
 *
 * 소유: Team B
 */

#ifndef SERVO_H
#define SERVO_H

#include "config.h"
#include "types.h"

/** 서보 초기화. Timer1 Fast PWM Mode 14 설정, 대기 각도(180도)로 정렬. */
void Servo_Init(void);

/** 슈퍼루프 tick. 점프/램프/안정대기 진행. */
void Servo_Task(void);

/**
 * 푸시 램프 시작: 180->50 즉시 점프 후 50->0 을 5도/500ms 로 하강.
 * 0도 도달 후에는 그 자리에 정지(홀드)하며 Servo_IsBusy() 가 false 로 떨어진다.
 * 복귀는 자동으로 하지 않는다 — 호출자가 Servo_ReturnIdle() 을 불러야 한다.
 * 이미 사이클(램프/복귀) 진행 중이면 무시한다.
 */
void Servo_StartPushRamp(void);

/**
 * 즉시 복귀: 현재 각도(램프 중이든 0도 홀드든)에서 곧바로 180도로 점프한 뒤
 * 안정대기 후 IDLE. 상위설계 §2.2 "IR 감지 시 즉시 서보 180도 복귀" 및
 * 각 차수의 "0도 도달 즉시 180도 복귀"에 사용한다.
 */
void Servo_ReturnIdle(void);

/** 사이클(램프 또는 복귀 점프+안정) 진행 중인가. 0도 홀드/180도 대기면 false. */
bool Servo_IsBusy(void);

#endif /* SERVO_H */
