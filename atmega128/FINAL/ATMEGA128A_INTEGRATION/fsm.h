#ifndef FSM_H
#define FSM_H

#include "types.h"

extern SystemCtx g_ctx;

void         Fsm_Init(void);                    /* g_ctx 초기화 */
void         Fsm_Task(void);                     /* 매 루프 호출. 도착/완료 감지 + RESULT 재전송 */
void         Fsm_HandleFrame(const Frame *f);    /* 파싱된 명령 1개 처리. 판정→상태별 분기 */
SystemState  Fsm_State(void);                     /* 현재 상태 조회 */
void         Fsm_SetState(SystemState next);      /* 상태 전이 (단일 진입점) */

#endif
