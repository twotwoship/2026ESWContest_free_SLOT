/**
 * @file    sensors.h
 * @brief   IR x2 (SEN0503) + 홈 센서 x2 (SG255) 통합 드라이버
 *
 * 기반: ATmega128A_펌웨어_식별자_v3_noEEPROM.md §4.8
 *
 * IR / 홈 센서를 한 모듈에 두는 이유:
 *   홈X(PD0/INT0), 홈Y(PD1/INT1), IR1(PD2/INT2) 이 EICRA 한 레지스터를 공유하고,
 *   같은 PORTD 안에서 풀업 정책이 반대다 (홈=외부 4.7k 풀업이라 내부 OFF,
 *   IR=NPN 오픈 컬렉터라 내부 ON). 모듈을 나누면 초기화 순서에 따라
 *   한쪽이 다른 쪽 설정을 통째로 덮어쓴다.
 *   IR2 는 EICRB/INT4 로 별개지만 대칭성을 위해 같은 파일에 둔다.
 *
 * [수정] SensorCtx 는 types.h 에 두지 않는다. sensors.c 내부 static 으로
 *        옮겨서 외부 노출을 막는다 (내부 context struct 는 .c 소유 원칙).
 *
 * 소유: Team B
 */

#ifndef SENSORS_H
#define SENSORS_H

#include "config.h"
#include "types.h"

/**
 * 센서 초기화.
 * DDR/풀업 -> EICRA/EICRB 트리거 조건 -> EIFR 클리어 -> EIMSK 순으로 설정한다.
 * 홈 인터럽트(INT0/INT1)는 여기서 활성화되고,
 * IR 인터럽트(INT2/INT4)는 비활성 상태로 시작한다 (Sensors_IrEnable 로 개폐).
 *
 * @note sei() 이전에 호출할 것.
 */
void Sensors_Init(void);

/**
 * 슈퍼루프 tick. 홈 센서 레벨 폴링만 담당한다 (IR 은 ISR 이 직접 래치하므로
 * Task 개입 없음). 1회 호출당 1ms 이내 반환.
 */
void Sensors_Task(void);

/* ---- IR (배출 감지) ----------------------------------------------------- */

/** IR 감지 래치 해제. DISPENSING 진입 직전(설계서 §6)에 호출한다. */
void Sensors_IrClear(void);

/**
 * IR 감지 여부 (설계서 §6: Falling Edge, HIGH->LOW).
 * @return IR1 또는 IR2 에 falling edge 가 한 번이라도 발생했으면 true.
 *         래치이므로 Sensors_IrClear() 전까지 true 를 유지한다.
 * @note 재확인/디바운스 없음. 빔을 3~8ms 만에 통과하는 빠른 낙하도 놓치지
 *       않기 위함이며, 전기적 노이즈는 설계서 §7 상 처리 범위 밖이다.
 */
bool Sensors_IrDetected(void);

/**
 * IR 인터럽트 개폐 (EIMSK 의 INT2/INT4 비트만 조작).
 * 설계서 §6: DISPENSING 동안만 활성, 그 외 상태에서는 닫아 둔다.
 * @param en true=활성화. 활성화 시 닫혀 있는 동안 쌓인 잔류 플래그(EIFR)를
 *           먼저 지워 여는 즉시 과거 엣지로 ISR 이 도는 것을 막는다.
 */
void Sensors_IrEnable(bool en);

/* ---- 홈 센서 (원점 감지) ------------------------------------------------ */

/**
 * 홈 센서 핀 레벨 직접 판독.
 * 이미 원점에 정지한 채로 부팅하면 신호가 활성 레벨로 고정되어
 * 엣지가 아예 발생하지 않는다. 엣지만 믿으면 그 경우 영원히 탐색한다.
 * @return 현재 원점 위에 있으면 true.
 */
bool Sensors_HomeLevel(AxisId axis);

/** 홈 센서 엣지 래치 확인 (ISR 또는 레벨 폴링이 세트). */
bool Sensors_HomeLatched(AxisId axis);

/** 홈 센서 래치 해제. 홈 탐색 시작 직전에 호출한다. */
void Sensors_HomeClear(AxisId axis);

#endif /* SENSORS_H */
