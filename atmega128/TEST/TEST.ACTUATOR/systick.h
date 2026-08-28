/**
 * @file    systick.h
 * @brief   1ms 시스템 틱 (Timer0 CTC) — 인터페이스
 *
 * 기반: ATmega128A_펌웨어_식별자_v3_noEEPROM.md §4.1
 *
 * ┌──────────────────────────────────────────────────────────────────────┐
 * │  [임시 파일 — Team A 소유]                                            │
 * │  분업 가이드라인 §1 에서 systick.c/h 는 Team A 소유다. 이 파일은      │
 * │  Team B(액추에이터/센서) 단독 빌드를 위해 저장소에 넣은 것이며,       │
 * │  통합 시 Team A 의 systick.c/h 로 교체한다. 인터페이스(아래 3함수 +   │
 * │  g_tick_ms)는 식별자 문서 §4.1 로 고정되어 있으므로 교체해도          │
 * │  Team B 코드는 그대로 링크된다.                                       │
 * │  ※ 스텁(가짜 구현)이 아니라 동작하는 실물이다 — 타이밍은 스텁 불가.   │
 * └──────────────────────────────────────────────────────────────────────┘
 */

#ifndef SYSTICK_H
#define SYSTICK_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Timer0 비교매치 ISR 이 1ms 마다 증가시키는 전역 틱 카운터.
 * 8비트 MCU 에서 32비트라 비원자적으로 읽힌다 — 직접 읽지 말고
 * 반드시 Systick_Now() 를 거칠 것.
 */
extern volatile uint32_t g_tick_ms;

/** Timer0 를 CTC 1ms 로 설정하고 비교매치 인터럽트를 허용한다. sei() 이전 호출. */
void Systick_Init(void);

/** g_tick_ms 를 인터럽트 차단 후 원자적으로 복사해 반환한다. */
uint32_t Systick_Now(void);

/**
 * since 시각으로부터 ms 밀리초가 경과했는지.
 * 부호 없는 뺄셈이라 g_tick_ms 오버플로(약 49.7일)에도 안전하다.
 */
bool Systick_Elapsed(uint32_t since, uint32_t ms);

#endif /* SYSTICK_H */
