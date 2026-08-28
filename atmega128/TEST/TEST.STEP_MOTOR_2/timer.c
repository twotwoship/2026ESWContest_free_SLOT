#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include "timer.h"

static volatile uint16_t g_tick_ms = 0;

void Timer2_Init(void)
{
    TCCR2 = (1 << WGM21) | (1 << CS22);   // CTC 모드, prescaler 64
    OCR2  = 249;                           // 250 카운트 -> 1ms (F_CPU=16MHz 기준)
    TIMSK |= (1 << OCIE2);
}

ISR(TIMER2_COMP_vect)
{
    g_tick_ms++;
}

uint16_t Tick_GetMs(void)
{
    uint16_t t;
    cli();
    t = g_tick_ms;
    sei();
    return t;
}
