#ifndef STEP_MOTOR_H
#define STEP_MOTOR_H

#include <stdint.h>

// 28BYJ-48 스텝당 각도: 정격 스텝각 5.625/64 -> 감속기 통과 후 1회전에 약 4096 스텝(Full Drive 기준)
// 슬롯 간 이동에 필요한 스텝 수는 실제 기구(피치, 벨트비 등)에 따라 다르므로 실측 후 조정 필요.
#define STEPPER_STEP_DELAY_MS   4        // 스텝 간 간격(ms) - 너무 짧으면 탈조 가능, 실측 후 조정

typedef enum { AXIS_X = 0, AXIS_Y = 1 } StepperAxis_t;

void Stepper_Init(void);

// 지정한 축을 dir 방향으로 steps 스텝만큼 이동 시작 (논블로킹)
// dir: 0 = 정방향, 1 = 역방향
void Stepper_MoveSteps(StepperAxis_t axis, uint8_t dir, uint16_t steps, uint16_t now_ms);

// 메인 루프에서 매 사이클 호출 - 두 축 모두 갱신
void Stepper_Update(uint16_t now_ms);

uint8_t Stepper_IsMoving(StepperAxis_t axis);
uint8_t Stepper_AnyMoving(void);   // 두 축 중 하나라도 이동 중이면 1

#endif // STEP_MOTOR_H
