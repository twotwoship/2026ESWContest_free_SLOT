/**
 * @file    main.c
 * @brief   BLSlot(약속) ATmega128A 메인 슈퍼루프
 *
 * 기반: ATmega128A_펌웨어_식별자_v3_noEEPROM.md §5 (main 스켈레톤) / 상위설계 §2.1
 *
 * 구조: RTOS 없는 단일 무한루프(슈퍼루프). 모든 *_Task() 는 "지금 할 일이
 * 있으면 조금만 하고 즉시 반환"하는 non-blocking tick 함수라는 전제다.
 * 어떤 Task() 도 안에서 블로킹 대기(_delay_ms, while)를 하면 안 된다 —
 * 그 순간 나머지 전부가 멈춘다 (UART 수신 유실, 동시 축 구동 불가).
 *
 * [수정] 식별자 문서 §5 스켈레톤에 맞춰 재정리:
 *   - Sys_Init() 안에서 sei() 이후 STATE_RECOVERY_REQUIRED 진입 + Homing_Start().
 *   - Task_Actuator() 는 타이밍 tick(Sensors/Stepper/Servo)은 항상 돌리고,
 *     시퀀스 컨트롤러(Homing_Task / Dispense_Task)는 해당 상태에서만 호출.
 *   이전 구현은 Homing_Task/Dispense_Task 를 상태 무관하게 매 루프 호출했다.
 *
 * [확인 필요] Uart_Init/Uart_Task, Fsm_Init/Fsm_Task/Fsm_State/Fsm_SetState 는
 * Team A 소유 함수다. 시그니처는 식별자 문서 §4.2/§4.4 를 따랐고, 실제
 * uart.h / fsm.h 가 나오면 include 와 호출부를 거기 맞게 조정한다.
 * (Team A 완성 전까지는 team_a_stub.c/h 로 링크 — 이 저장소엔 미포함.)
 */

#include "config.h"
#include "types.h"

#include "systick.h"   /* Team A */
#include "uart.h"      /* Team A - [확인 필요] 시그니처 가정 */
#include "fsm.h"       /* Team A - [확인 필요] 시그니처 가정 */

#include "sensors.h"   /* Team B */
#include "stepper.h"
#include "servo.h"
#include "homing.h"
#include "dispense.h"

#include <avr/interrupt.h>

static void Sys_Init(void);
static void Task_Actuator(void);

int main(void)
{
    Sys_Init();

    for (;;) {
        Uart_Task();        /* Team A: RX 링버퍼 비우기 -> 파싱 -> Fsm_HandleFrame */
        Task_Actuator();    /* Team B: 물리 동작 tick (상태 종속) */
        Fsm_Task();         /* Team A: 상태 전이 판정 + RESULT 재전송 타이머 */
    }
}

static void Sys_Init(void)
{
    /* 순서: 시간 기준(Systick) -> 통신(UART) -> 액추에이터(Team B) -> FSM.
     * 인터럽트 마스크를 세우는 Sensors_Init() 등은 전부 sei() 이전에
     * 끝내야 한다 (각 모듈 헤더에 명시된 전제). */
    Systick_Init();
    Uart_Init();

    Sensors_Init();
    Stepper_Init();
    Servo_Init();

    Fsm_Init();

    sei();   /* 전역 인터럽트 활성화. 이후부터 ISR 이 실제로 돈다 */

    /* 상위설계 §2.1: 전원 인가 시 무조건 RECOVERY_REQUIRED 진입 후 홈 탐색.
     * 상태 진입은 FSM(Team A) 소유지만, 부팅 시 1회 강제 진입은 여기서
     * 명시한다 (식별자 문서 §5 스켈레톤). 완료 판정/명령 게이팅은 FSM 이
     * Homing_IsComplete() / Homing_Status() 로 스스로 한다. */
    Fsm_SetState(STATE_RECOVERY_REQUIRED);
    Homing_Start();

    /* 부팅 직후 쓰레기 프레임에 ERROR 로 응답하지 않도록 하는
     * boot_suppress_error 억제 플래그는 Fsm_Init() (Team A) 이 자체적으로
     * 세팅하는 것으로 가정한다 (식별자 문서 §2.2 SystemCtx). */
}

static void Task_Actuator(void)
{
    /* 타이밍 기반 tick 은 상태와 무관하게 항상 진행
     * (홈 센서 레벨 폴링 / 서보 PWM 램프·복귀 / 스텝 간격). */
    Sensors_Task();
    Stepper_Task();
    Servo_Task();

    /* 시퀀스 컨트롤러는 해당 상태에서만 (식별자 문서 §5). */
    switch (Fsm_State()) {
    case STATE_RECOVERY_REQUIRED:
        Homing_Task();
        break;
    case STATE_DISPENSING:
        Dispense_Task();
        break;
    default:
        break;
    }
}
