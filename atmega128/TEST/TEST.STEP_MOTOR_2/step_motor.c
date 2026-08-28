#include <avr/io.h>
#include "step_motor.h"

// Full Drive (2상 여자) 풀스텝 시퀀스 - 항상 코일 2개 동시 여자
static const uint8_t FULL_STEP[4] = {
    0b0011,
    0b0110,
    0b1100,
    0b1001
};

typedef struct {
    uint8_t  dir;              // 0: 정방향, 1: 역방향
    uint8_t  step_index;
    uint16_t last_step_tick;
    uint16_t step_count;       // 이번 이동에서 지금까지 진행한 스텝 수
    uint16_t target_steps;     // 이번 이동에서 목표 스텝 수
    uint8_t  moving;
} stepper_t;

static stepper_t s_stepper[2];   // [0]=AXIS_X, [1]=AXIS_Y

// X축은 PORTA 하위 니블(PA0~3), Y축은 상위 니블(PA4~7)
static void motor_write(StepperAxis_t axis, uint8_t pattern)
{
    if (axis == AXIS_X)
    {
        PORTA = (uint8_t)((PORTA & 0xF0) | (pattern & 0x0F));
    }
    else
    {
        PORTA = (uint8_t)((PORTA & 0x0F) | ((pattern << 4) & 0xF0));
    }
}

void Stepper_Init(void)
{
    DDRA  |= 0xFF;   // PA0~PA7 전부 출력 (X: PA0~3, Y: PA4~7)
    PORTA  = 0x00;

    for (uint8_t i = 0; i < 2; i++)
    {
        s_stepper[i].moving = 0;
        s_stepper[i].step_index = 0;
        s_stepper[i].step_count = 0;
        s_stepper[i].target_steps = 0;
    }
}

void Stepper_MoveSteps(StepperAxis_t axis, uint8_t dir, uint16_t steps, uint16_t now_ms)
{
    stepper_t *s = &s_stepper[axis];

    s->dir            = dir;
    s->step_count      = 0;
    s->target_steps    = steps;
    s->last_step_tick  = now_ms;
    s->moving          = (steps > 0) ? 1 : 0;
}

static void stepper_update_one(StepperAxis_t axis, stepper_t *s, uint16_t now_ms)
{
    if (!s->moving)
    {
        return;
    }

    if ((uint16_t)(now_ms - s->last_step_tick) >= STEPPER_STEP_DELAY_MS)
    {
        s->last_step_tick = now_ms;

        if (s->dir == 0)
        {
            s->step_index = (uint8_t)((s->step_index + 1) % 4);
        }
        else
        {
            s->step_index = (uint8_t)((s->step_index + 3) % 4);   // -1 mod 4
        }

        motor_write(axis, FULL_STEP[s->step_index]);
        s->step_count++;

        if (s->step_count >= s->target_steps)
        {
            s->moving = 0;
            // 코일 통전을 계속 유지할지(정지 토크) 끊을지는 기구 설계에 따라 선택.
            // 여기서는 발열/소비전력을 줄이기 위해 정지 시 코일을 끈다.
            motor_write(axis, 0x00);
        }
    }
}

void Stepper_Update(uint16_t now_ms)
{
    stepper_update_one(AXIS_X, &s_stepper[AXIS_X], now_ms);
    stepper_update_one(AXIS_Y, &s_stepper[AXIS_Y], now_ms);
}

uint8_t Stepper_IsMoving(StepperAxis_t axis)
{
    return s_stepper[axis].moving;
}

uint8_t Stepper_AnyMoving(void)
{
    return s_stepper[AXIS_X].moving || s_stepper[AXIS_Y].moving;
}
