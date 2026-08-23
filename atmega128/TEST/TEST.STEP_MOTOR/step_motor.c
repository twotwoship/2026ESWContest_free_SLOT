/*
 * step_motor.c
 * 28BYJ-48 + ULN2003 스테퍼 모터 제어 (Full Drive 풀스텝)
 *
 * 배선:
 *   PA0 -> ULN2003 IN1
 *   PA1 -> ULN2003 IN2
 *   PA2 -> ULN2003 IN3
 *   PA3 -> ULN2003 IN4
 */

/*
 * 스테퍼 모터 여자(勵磁) 방식 정리 (IN1~IN4 기준)
 *
 * - Wave Drive (1상 여자)
 *   한 번에 코일 1개만 여자: 1000 -> 0100 -> 0010 -> 0001 -> ...
 *   소비 전류가 적지만 토크가 약함 (Full Drive 대비 절반 수준).
 *
 * - Full Drive (2상 여자, 본 코드에서 사용)
 *   항상 인접한 코일 2개를 동시에 여자: 0011 -> 0110 -> 1100 -> 1001 -> ...
 *   Wave Drive보다 토크가 크고 안정적으로 회전 (소비 전류는 더 큼).
 *   스텝 각도는 Wave Drive와 동일 (예: 28BYJ-48 기준 스텝당 각도 동일).
 *
 * - Half Drive (1-2상 여자, 하프스텝)
 *   1상 여자와 2상 여자를 번갈아 사용: 1000 -> 1100 -> 0100 -> 0110 -> ...
 *   스텝 수가 2배로 늘어나는 대신 분해능이 2배로 향상되고 회전이 더 부드러움
 *   (토크는 여자 코일 수에 따라 스텝마다 다소 변동).
 */
#include <avr/io.h>
#include "step_motor.h"

// Full Drive (2상 여자) 풀스텝 시퀀스 (IN1~IN4) - 항상 코일 2개 동시 여자, Wave Drive 대비 토크 ↑
static const uint8_t FULL_STEP[4] = {
	0b0011,
	0b0110,
	0b1100,
	0b1001
};

typedef struct {
	uint8_t  dir;             // 0: 정방향, 1: 역방향
	uint8_t  step_index;
	uint16_t last_step_tick;
	uint16_t step_count;      // 이번 회전 시작 이후 누적 스텝 수
	uint8_t  moving;
} stepper_t;

static stepper_t s_stepper;

static void motor_write(uint8_t pattern)
{
	PORTA = (PORTA & 0xF0) | (pattern & 0x0F);
}

void Stepper_Init(void)
{
	DDRA  |= 0x0F;   // PA0~PA3 출력
	PORTA &= 0xF0;

	s_stepper.moving = 0;
	s_stepper.step_index = 0;
}

void Stepper_StartContinuous(uint8_t dir, uint16_t now_ms)
{
	s_stepper.dir            = dir;
	s_stepper.step_count     = 0;
	s_stepper.last_step_tick = now_ms;
	s_stepper.moving         = 1;
}

void Stepper_Stop(void)
{
	s_stepper.moving = 0;
}

void Stepper_Update(uint16_t now_ms)
{
	if (!s_stepper.moving)
	{
		return;
	}

	// STEPPER_STEP_DELAY_MS 마다 1스텝 진행
	if ((uint16_t)(now_ms - s_stepper.last_step_tick) >= STEPPER_STEP_DELAY_MS)
	{
		s_stepper.last_step_tick = now_ms;

		if (s_stepper.dir == 0)
		{
			s_stepper.step_index = (s_stepper.step_index + 1) % 4;
		}
		else
		{
			s_stepper.step_index = (s_stepper.step_index + 3) % 4; // -1 mod 4
		}

		motor_write(FULL_STEP[s_stepper.step_index]);
		s_stepper.step_count++;
	}
}

uint8_t Stepper_IsMoving(void)
{
	return s_stepper.moving;
}

uint16_t Stepper_GetStepCount(void)
{
	return s_stepper.step_count;
}
