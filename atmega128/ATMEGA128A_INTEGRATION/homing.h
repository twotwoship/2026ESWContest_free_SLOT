/**
 * @file    homing.h
 * @brief   전원 인가 시 무조건 X/Y 동시 홈 탐색
 *
 * 기반: 상위설계 §2.1 / §7, 식별자 문서 §4.9
 *
 * 확정 아키텍처 (상위설계 §7): 매 부팅마다 명령 수락 전에 X축과 Y축을
 * "동시에" 원점 방향으로 이동시킨다.
 *   1. X축 홈 센서 감지 -> X축 정지 -> Y축은 계속
 *   2. Y축 홈 센서 감지 -> Y축 정지 -> X축은 계속
 *   X/Y 모두 감지되면 그 지점을 원점 (0,0) 으로 확정.
 * Open-loop 스테퍼는 스스로 위치를 검증할 수 없으므로 물리 홈 센서만이
 * 신뢰 가능한 기준점이다.
 *
 * [수정] 이전 구현은 X 완료 후 Y 로 넘어가는 순차 탐색이었다. 상위설계
 *        §2.1 "X축, Y축 동시 진행" / §7 "동시에 원점 방향으로 이동" 에
 *        맞춰 동시 구동으로 재구현한다.
 *
 * 스텝 예산(HOMING_MAX_STEPS_X/Y) 내에 어느 한 축이라도 센서를 못 찾으면
 * 두 축 모두 정지 + 코일 해제하고 HOMING_MANUAL_WAIT 로 전환한다 (상위설계
 * §2.1). 사람이 두 축을 모두 원점으로 밀어 두 센서가 모두 감지되면 (0,0)
 * 을 확정하고 자동으로 완료된다. RECOVERY_REQUIRED 탈출은 이 홈 센서 감지
 * 성공뿐이다.
 *
 * 소유: Team B
 */

#ifndef HOMING_H
#define HOMING_H

#include "config.h"
#include "types.h"

/**
 * 홈 탐색 시작/재시작. 두 축을 동시에 홈 방향으로 구동한다.
 * 이미 원점 위에 있는 축은(Sensors_HomeLevel) 즉시 완료 처리한다.
 */
void Homing_Start(void);

/** 슈퍼루프 tick. */
void Homing_Task(void);

/** X, Y 두 축 모두 완료됐는가. */
bool Homing_IsComplete(void);

/** 현재 홈 탐색 상태 (HOMING_SEARCHING / HOMING_MANUAL_WAIT / HOMING_DONE). */
HomingStatus Homing_Status(void);

#endif /* HOMING_H */
