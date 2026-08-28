// =====================================================================
// fsm.c
// =====================================================================

#include <string.h>

#include "fsm.h"
#include "config.h"
#include "systick.h"
#include "protocol.h"
#include "stepper.h"
#include "dispense.h"
#include "homing.h"

SystemCtx g_ctx; //시스템의 상태와 동기화 정보

static bool Fsm_CheckIdConflict(const Frame *f);
static bool Fsm_IsDuplicate(const Frame *f);
static void Fsm_HandleMove(const Frame *f);
static void Fsm_HandleDispense(const Frame *f);
static void Fsm_HandleTimeout(const Frame *f);
static void Fsm_HandleAck(const Frame *f);
static void Fsm_ServiceResultRetx(void);

//FSM 초기화
void Fsm_Init(void)
{
    memset(&g_ctx, 0, sizeof(g_ctx)); 
    g_ctx.state = STATE_IDLE; //실제로는 main.c가 부팅 직후 RECOVERY_REQUIRED로 재설정
}

//현재 STATE 반환
SystemState Fsm_State(void)
{
    return g_ctx.state; 
}

//다음 STATE로 전환
void Fsm_SetState(SystemState next)
{
    g_ctx.state = next;
}

//동일한 요청번호가 다른 Payload와 함께 재사용됐는지 확인
//같은 req_id + 다른 Payload -> ID_CONFLICT (처음 보는 req_id -> 캐시에 기록하고 false
static bool Fsm_CheckIdConflict(const Frame *f)
{
    LastCmdRecord *rec = (f->cmd == CMD_MOVE) ? &g_ctx.last_move : &g_ctx.last_dispense;

    if (!rec) return false; //충돌 판정 대상 아님

    if (rec->seen && rec->req_id == f->req_id) {
        bool same_payload = (rec->x == f->x) && (rec->y == f->y) &&
                             (f->cmd != CMD_MOVE || rec->allow_time_sec == f->allow_time_sec);
        return !same_payload; //Payload 다르면 ID_CONFLICT
    }

    //처음 보는 요청번호 -> 캐시 갱신
    rec->req_id = f->req_id;
    rec->x = f->x;
    rec->y = f->y;
    rec->allow_time_sec = (f->cmd == CMD_MOVE) ? f->allow_time_sec : 0UL;
    rec->seen = true;
    return false;
}

//수신한 명령이 처리 중인 사이클의 요청번호와 동일한지(재전송인지) 확인
static bool Fsm_IsDuplicate(const Frame *f)
{
    return f->req_id == g_ctx.active_req_id;
}

//수신 처리 - 한 프레임을 받아 상태머신에 반영
void Fsm_HandleFrame(const Frame *f)
{
    if (!f->valid) { //구문이 유효하지 않으면
        if (!g_ctx.boot_suppress_error) {
            Protocol_SendInvalidFormat(); //잘못된 포맷
        }
        return;
    }

    switch (f->cmd) {
    case CMD_MOVE:
    case CMD_DISPENSE:
        //좌표 범위 확인
        if (f->x > COORD_X_MAX || f->y > COORD_Y_MAX) {
            Protocol_SendError(f->req_id, ERR_INVALID_COORD); //유효하지 않은 좌표
            return;
        }
        //MOVE의 ALLOW_TIME 범위 확인
        if (f->cmd == CMD_MOVE &&
            (f->allow_time_sec < ALLOW_TIME_MIN_SEC || f->allow_time_sec > ALLOW_TIME_MAX_SEC)) {
            Protocol_SendError(f->req_id, ERR_INVALID_TIME); //유효하지 않은 시간
            return;
        }
        //ID_CONFLICT 검사
        if (Fsm_CheckIdConflict(f)) {
            Protocol_SendError(f->req_id, ERR_ID_CONFLICT); //같은 요청번호 재사용
            return;
        }
		//정상 처리
        if (f->cmd == CMD_MOVE) {
            Fsm_HandleMove(f);
        } else {
            Fsm_HandleDispense(f);
        }
        break;

    case CMD_TIMEOUT:
        Fsm_HandleTimeout(f);
        break;

    case CMD_ACK:
        Fsm_HandleAck(f);
        break;

    default:
        break;
    }
}

//MOVE 처리
static void Fsm_HandleMove(const Frame *f)
{
    switch (g_ctx.state) {

	//대기 상태
    case STATE_IDLE:
        if (!Stepper_MoveToSlot(f->x, f->y)) {
            Protocol_SendError(f->req_id, ERR_STEPPER_ERROR);
            break;
        }

        g_ctx.active_req_id = f->req_id;
        g_ctx.pending_x      = f->x;
        g_ctx.pending_y      = f->y;

        Protocol_SendAck(f->req_id, TOK_MOVE);	//MOVE 명령을 정상적으로 접수한 후 ACK
        Fsm_SetState(STATE_MOVING);
        break;

    case STATE_MOVING:
        if (Fsm_IsDuplicate(f)) { //동일한 MOVE가 재전송된 경우 
            Protocol_SendAck(f->req_id, TOK_MOVE); //ACK만 재전송
        } else { 
            Protocol_SendError(f->req_id, ERR_BUSY);
        }
        break;

    case STATE_AWAITING_DISPENSE:
        if (Fsm_IsDuplicate(f)) { //완료된 MOVE가 재전송된 경우
            Protocol_SendWait(f->req_id); //현재 배출 대기 상태를 다시 알려줌
        } else {
            Protocol_SendError(f->req_id, ERR_BUSY);
        }
        break;

    case STATE_DISPENSING:
    case STATE_AWAITING_RESULT_ACK:
        Protocol_SendError(f->req_id, ERR_BUSY);
        break;

    case STATE_RECOVERY_REQUIRED:
        Protocol_SendError(f->req_id, ERR_RECOVERY_REQUIRED);
        break;

    default:
        break;
    }
}

//DISPENSE 처리
static void Fsm_HandleDispense(const Frame *f)
{
    switch (g_ctx.state) {

    case STATE_IDLE:
        //무시
        break;

    case STATE_MOVING: //WAIT 이전 DISPENSE
        Protocol_SendError(f->req_id, ERR_NOT_READY);
        break;

    case STATE_AWAITING_DISPENSE:
        //DISPENSE의 좌표는 직전에 수행한 MOVE의 좌표와 일치해야 함
        if (f->x == g_ctx.pending_x && f->y == g_ctx.pending_y) {
            g_ctx.active_req_id = f->req_id;

            Protocol_SendAck(f->req_id, TOK_DISPENSE);	//배출 동작을 시작
            Dispense_Start(f->x, f->y);
            Fsm_SetState(STATE_DISPENSING);
        } else {
            Protocol_SendError(f->req_id, ERR_COORD_MISMATCH);
        }
        break;

    case STATE_DISPENSING:
        if (Fsm_IsDuplicate(f)) { //동일한 DISPENSE가 재전송
            Protocol_SendAck(f->req_id, TOK_DISPENSE);	//ACK만 재전송
        } else {
            Protocol_SendError(f->req_id, ERR_BUSY);
        }
        break;

    case STATE_AWAITING_RESULT_ACK:
        if (f->req_id == g_ctx.result_req_id) { //동일한 DISPENSE가 재전송 -> 결과를 다시 전송
            Protocol_SendResult(g_ctx.result_req_id, g_ctx.cur_x, g_ctx.cur_y, g_ctx.cached_result);
        } else {
            Protocol_SendError(f->req_id, ERR_BUSY);
        }
        break;

    case STATE_RECOVERY_REQUIRED:
        Protocol_SendError(f->req_id, ERR_RECOVERY_REQUIRED);
        break;

    default:
        break;
    }
}

//TIMEOUT 처리
static void Fsm_HandleTimeout(const Frame *f)
{
    switch (g_ctx.state) {

    case STATE_AWAITING_DISPENSE:
        //현재 활성화된 MOVE의 요청번호와 일치할 때만 배출 대기를 종료
        if (f->req_id == g_ctx.active_req_id) {
            Protocol_SendAck(f->req_id, TOK_TIMEOUT);
            Fsm_SetState(STATE_IDLE);
        }
        //req_id 불일치시 무응답
        break;

    case STATE_IDLE:
        Protocol_SendAck(f->req_id, TOK_TIMEOUT);
        break;

    default:
        break;	//그 외 상태 -> 무응답
    }
}

//ACK 처리
static void Fsm_HandleAck(const Frame *f)
{
    //AWAITING_RESULT_ACK 상태에서만 ACK 처리
    if (g_ctx.state != STATE_AWAITING_RESULT_ACK) return;	//그 외 상태는 무응답

    //현재 전송 중인 RESULT의 요청번호와 일치해야 함
    if (f->req_id != g_ctx.result_req_id) return;

    //RESULT가 정상적으로 수신되면 IDLE 상태로 전환
    Fsm_SetState(STATE_IDLE);
}

//RESULT 재전송 처리 - 재전송 주기가 지났는지 확인하고, RESULT를 10초 주기로 최대 6회까지 재전송
//(6회를 넘어서면 강제로 STATE_IDLE로 전이)
static void Fsm_ServiceResultRetx(void)
{
    if (!Systick_Elapsed(g_ctx.result_retx_tick_ms, RESULT_RETX_INTERVAL_MS)) return;

    if (g_ctx.result_retx_count < RESULT_RETX_MAX_COUNT) {
        g_ctx.result_retx_count++;
        g_ctx.result_retx_tick_ms = Systick_Now();
        Protocol_SendResult(g_ctx.result_req_id, g_ctx.cur_x, g_ctx.cur_y, g_ctx.cached_result);
    } else {
        //최대 6회 재전송에도 ACK 없음 -> 추가 송신 없이 강제 IDLE 전이
        Fsm_SetState(STATE_IDLE);
    }
}

//주기적 FSM 처리 - 매 루프마다 폴링해서 도착/완료 감지 & RESULT 재전송
void Fsm_Task(void)
{
    switch (g_ctx.state) {

    case STATE_MOVING:
        //MOVE 완료 처리
        if (!Stepper_IsBusy()) {
            g_ctx.cur_x = g_ctx.pending_x;	//스텝모터가 목표 위치에 도착
            g_ctx.cur_y = g_ctx.pending_y;

            Protocol_SendWait(g_ctx.active_req_id);	//해당 위치가 배출 준비 완료임을 알림
            Fsm_SetState(STATE_AWAITING_DISPENSE);
        }
        break;

    case STATE_DISPENSING:
        //DISPENSE 완료 처리
        if (Dispense_IsComplete()) {
            //배출 결과를 저장. RESULT ACK를 받을 때까지 재전송할 수 있도록 캐시
            g_ctx.cached_result       = Dispense_Result();
            g_ctx.result_req_id       = g_ctx.active_req_id;
            g_ctx.result_retx_count   = 0;
            g_ctx.result_retx_tick_ms = Systick_Now();

            Protocol_SendResult(g_ctx.result_req_id, g_ctx.cur_x, g_ctx.cur_y, g_ctx.cached_result);
            Fsm_SetState(STATE_AWAITING_RESULT_ACK);	//첫 번째 RESULT를 전송
        }
        break;

    case STATE_AWAITING_RESULT_ACK:
        Fsm_ServiceResultRetx();
        break;

    case STATE_RECOVERY_REQUIRED:
        //복구 완료 처리
        if (Homing_IsComplete()) {
            g_ctx.cur_x = 0;			//홈 복귀 완료 후 현재 좌표를 원점으로 설정
            g_ctx.cur_y = 0;
            g_ctx.boot_suppress_error = false;
            Fsm_SetState(STATE_IDLE);
        }
        break;

    default:
        break;
    }
}
