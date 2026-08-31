/**
 * @file    main.c
 * @brief   약속(藥SLOT) ATmega128A — Team A(UART/FSM) + Team B(액추에이터/센서) 통합 슈퍼루프
 *
 * 기반:
 *   - 상위설계  README_1.md §2.1(부팅), §9(시나리오)
 *   - 식별자문서 README_2.md §4.10(main 함수), §5(스켈레톤)
 *   - 분업가이드 README_3.md §3(얼린 인터페이스), §5(통합 순서)
 *
 * ┌──────────────────────────────────────────────────────────────────────┐
 * │  통합 산출물 — AT_UART/ 와 AT_ACTUATOR/ 를 합친 결과.                  │
 * │  main.c 는 어느 팀도 완성본을 제출하지 않아 이 파일에서 새로 작성.    │
 * │    - AT_UART/main.c      : Atmel Studio 기본 템플릿(빈 껍데기)         │
 * │    - AT_ACTUATOR/main.c  : Team B 단독 빌드용. Uart_Task() 라는        │
 * │                            존재하지 않는 함수를 호출(임시 스텁 전제)  │
 * │  두 팀 소스는 한 줄도 수정하지 않았다. 이 파일이 유일한 접착층이다.   │
 * │  발견된 상위설계 불일치는 INTEGRATION_NOTES.md 에 정리(수정 안 함).   │
 * └──────────────────────────────────────────────────────────────────────┘
 *
 * 슈퍼루프 규약(README_2 §5): 모든 *_Task() 는 논블로킹 tick. 1회 호출당
 * 1ms 이내 반환. 블로킹 대기(_delay_ms/while)는 Sys_Init() 안에서만 허용.
 */

#include "config.h"
#include "types.h"

#include "systick.h"    /* Team A 소유 — systick.c 프리스케일러 1줄만 통합에서 수정(B-1). NOTES §B-1 */
#include "uart.h"       /* Team A */
#include "protocol.h"   /* Team A */
#include "fsm.h"        /* Team A — extern SystemCtx g_ctx */

#include "sensors.h"    /* Team B */
#include "stepper.h"    /* Team B */
#include "servo.h"      /* Team B */
#include "homing.h"     /* Team B */
#include "dispense.h"   /* Team B */

#include <avr/interrupt.h>

static void Sys_Init(void);
static void Task_UartRx(void);
static void Task_Actuator(void);

/* ==========================================================================
 *  슈퍼루프
 * ========================================================================== */

int main(void)
{
    Sys_Init();

    for (;;) {
        Task_UartRx();      /* 라인 추출 -> 파싱 -> Fsm_HandleFrame (README_2 §4.10) */
        Task_Actuator();    /* 물리 동작 tick (상태 종속) */
        Fsm_Task();         /* 상태 전이 판정 + RESULT 재전송 타이머 */
    }
}

/* ==========================================================================
 *  초기화
 * --------------------------------------------------------------------------
 *  순서: 시간기준(Systick) -> 통신(UART) -> 센서 -> 액추에이터 -> FSM.
 *  인터럽트 마스크를 세우는 Sensors_Init() 등은 전부 sei() 이전에 끝낸다
 *  (각 모듈 헤더의 전제). README_2 §5 스켈레톤과 동일하되, 센서 초기화를
 *  스테퍼보다 먼저 두어 EICRA/EICRB 설정이 다른 초기화에 묻히지 않게 한다.
 * ========================================================================== */

static void Sys_Init(void)
{
    Systick_Init();
    Uart_Init();

    Sensors_Init();
    Stepper_Init();
    Servo_Init();

    Fsm_Init();

    sei();   /* 이후부터 ISR(USART0_RX / TIMER0_COMP / INT0·1) 이 실제로 돈다 */

    /* README_2 §5: 부팅 직후 쓰레기 프레임에 ERROR 로 응답하지 않도록 억제.
     * [불일치 — NOTES §2] Team A 의 Fsm_Init() 은 memset 으로 g_ctx 를 0 으로
     * 밀 뿐 이 플래그를 true 로 세우지 않는다. AT_ACTUATOR/main.c 는 Fsm_Init()
     * 이 자체적으로 세팅한다고 가정했었다. 스켈레톤(§5)대로 여기서 명시한다.
     * Fsm_Task() 가 홈 탐색 완료 시 이 플래그를 false 로 내린다. */
    g_ctx.boot_suppress_error = true;

    /* 상위설계 §2.1: 전원 인가 시 무조건 RECOVERY_REQUIRED 진입 후 홈 탐색.
     * 완료 판정/IDLE 전이는 Fsm_Task() 가 Homing_IsComplete() 로 스스로 한다. */
    Fsm_SetState(STATE_RECOVERY_REQUIRED);
    Homing_Start();
}

/* ==========================================================================
 *  수신 처리 (README_2 §4.10 Task_UartRx)
 * --------------------------------------------------------------------------
 *  AT_ACTUATOR/main.c 가 부르던 Uart_Task() 는 Team A 인터페이스에 없다.
 *  Team A 는 §4.2 대로 Uart_ReadLine() 만 제공하고, "라인 -> 파싱 ->
 *  Fsm_HandleFrame" 조립은 main 이 한다(§4.10). 그 규약을 그대로 구현한다.
 *
 *  버퍼 크기 = FRAME_MAX_LEN + 1 (65):
 *    Protocol_Parse() 는 내용 길이 > FRAME_MAX_LEN-1(63) 이면 INVALID_FORMAT.
 *    버퍼를 64 로 잡으면 Uart_ReadLine() 이 63 자로 잘라 담아 길이검사(>63)를
 *    아슬아슬하게 통과할 수 있다(= Team A uart.h 주석의 "잘린 결과는 항상
 *    FRAME_MAX_LEN 초과" 전제가 성립 안 함, NOTES §3). 65 로 잡으면 64 자
 *    이상 라인이 항상 길이검사에서 걸린다.
 *
 *  Protocol_Parse() 실패 시에도 Fsm_HandleFrame(&f) 를 호출한다 — f.valid 가
 *  false 이므로 FSM 이 ERROR|INVALID_FORMAT 로 응답한다(부팅 억제 중이면 무시).
 * ========================================================================== */

static void Task_UartRx(void)
{
    char  line[FRAME_MAX_LEN + 1U];
    Frame f;

    if (!Uart_ReadLine(line, (uint8_t)sizeof line)) {
        return;   /* 완성된 라인 없음 */
    }

    (void)Protocol_Parse(line, &f);
    Fsm_HandleFrame(&f);
}

/* ==========================================================================
 *  액추에이터 tick (README_2 §5 / AT_ACTUATOR/main.c 와 동일 게이팅)
 * --------------------------------------------------------------------------
 *  타이밍 기반 tick(Sensors/Stepper/Servo)은 상태와 무관하게 항상 진행.
 *  시퀀스 컨트롤러(Homing/Dispense)는 해당 상태에서만 호출한다.
 * ========================================================================== */

static void Task_Actuator(void)
{
    Sensors_Task();
    Stepper_Task();
    Servo_Task();

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
