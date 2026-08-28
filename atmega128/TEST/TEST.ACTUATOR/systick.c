/**
 * @file    systick.c
 * @brief   1ms 시스템 틱 (Timer0 CTC) — 실물 구현
 *
 * ┌──────────────────────────────────────────────────────────────────────┐
 * │  [임시 파일 — Team A 소유]  systick.h 상단 배너 참조.                 │
 * │  통합 시 Team A 의 systick.c 로 교체. 스텁이 아니라 실동작 코드다.    │
 * └──────────────────────────────────────────────────────────────────────┘
 *
 * Timer0 (ATmega128, 8비트): CTC 모드, prescaler 64, OCR0 = SYSTICK_OCR0(249).
 *   16MHz / 64 / (249 + 1) = 1000 Hz  ->  1ms 마다 TIMER0_COMP 인터럽트.
 *
 * 타이머 자원 배정 (식별자 문서 §0):
 *   Timer0 = 1ms 시스템 틱 (여기)   Timer1 = 서보 PWM (servo.c)   Timer2 = 예비
 *
 * 비트 조작은 _BV() 대신 (1 << n) 을 직접 쓴다 (프로젝트 컨벤션).
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/atomic.h>

#include "systick.h"
#include "config.h"   /* SYSTICK_OCR0 */

volatile uint32_t g_tick_ms = 0UL;

void Systick_Init(void)
{
    /* TCCR0: WGM01=1 (CTC), CS02:00 = 100 (clk/64).
     * ATmega128 Timer0 프리스케일러 표: 100 = /64. */
    TCCR0 = (uint8_t)((1U << WGM01) | (1U << CS02));
    OCR0  = (uint8_t)SYSTICK_OCR0;
    TCNT0 = 0U;

    TIMSK |= (uint8_t)(1U << OCIE0);   /* 비교매치 인터럽트 허용 */
}

uint32_t Systick_Now(void)
{
    uint32_t v;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        v = g_tick_ms;
    }
    return v;
}

bool Systick_Elapsed(uint32_t since, uint32_t ms)
{
    return (uint32_t)(Systick_Now() - since) >= ms;
}

/** 1ms 마다 진입. 카운터 증가 한 줄만 — 다른 타이밍은 전부 여기서 파생. */
ISR(TIMER0_COMP_vect)
{
    g_tick_ms++;
}
