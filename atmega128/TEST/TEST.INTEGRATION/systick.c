// =====================================================================
// systick.c
// =====================================================================

#include <avr/io.h>
#include <avr/interrupt.h>
#include "config.h"
#include "systick.h"
volatile uint32_t g_tick_ms = 0UL;
ISR(TIMER0_COMP_vect)
{
    ++g_tick_ms;
}
void Systick_Init(void)
{
	TCNT0  = 0; //카운터 리셋
    TCCR0 = (1U << WGM01) | (1U << CS01) | (1U << CS00); //CTC모드, 64분주 설정
    OCR0 = (uint8_t)SYSTICK_OCR0; //249
    TIMSK |= (1U << OCIE0);
}
uint32_t Systick_Now(void) //현재 시간
{
	//8비트 MCU에서 32비트 변수 읽기(데이터 깨짐 현상 방지)
	
    uint32_t value;
    uint8_t sreg = SREG; //현재 인터럽트 상태 저장
    cli();	//전역 인터럽트 차단
    value = g_tick_ms;	//안전하게 변수 복사
    SREG = sreg;	//이전 인터럽트 상태 복원
    return value;	//복사 값 반환
}
//since 로부터 ms 경과했는지
bool Systick_Elapsed(uint32_t since, uint32_t ms) //기준 시간, 기다리고 싶은 ms
{
    return (uint32_t)(Systick_Now() - since) >= ms;
}
