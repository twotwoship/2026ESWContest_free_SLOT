#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void Timer2_Init(void);          // Timer2 CTC, 1ms 마다 tick 증가
uint16_t Tick_GetMs(void);       // 인터럽트 안전하게 현재 tick(ms) 읽기

#endif // TIMER_H
