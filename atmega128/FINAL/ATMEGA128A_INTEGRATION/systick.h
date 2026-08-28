#ifndef SYSTICK_H
#define SYSTICK_H

#include <stdint.h>
#include <stdbool.h>

extern volatile uint32_t g_tick_ms;
void Systick_Init(void);
uint32_t Systick_Now(void);
bool Systick_Elapsed(uint32_t since, uint32_t ms);

#endif /* SYSTICK_H */
