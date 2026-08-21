/*
 * step_motor.h
 * 28BYJ-48 + ULN2003 스테퍼 모터 제어 (Full Drive 풀스텝)
 *
 * 배선:
 *   PA0 -> ULN2003 IN1
 *   PA1 -> ULN2003 IN2
 *   PA2 -> ULN2003 IN3
 *   PA3 -> ULN2003 IN4
 */
#ifndef STEP_MOTOR_H_
#define STEP_MOTOR_H_

#include <stdint.h>

// 스텝 사이 딜레이(ms) - 모터 회전 속도 (작을수록 빠름, 너무 작으면 탈조 위험)
#define STEPPER_STEP_DELAY_MS 3

void     Stepper_Init(void);
void     Stepper_StartContinuous(uint8_t dir, uint16_t now_ms);
void     Stepper_Stop(void);
void     Stepper_Update(uint16_t now_ms);
uint8_t  Stepper_IsMoving(void);
uint16_t Stepper_GetStepCount(void);

#endif /* STEP_MOTOR_H_ */
