#include "uart_protocol.h"
#include "eeprom_state.h"

extern PersistState_t g_persist_state;

// ==================== FSM 핸들러 (임시 스텁) ====================
// EEPROM 로드/저장 배선은 연결해뒀지만, 상태 전이·중복검사(ID_CONFLICT 등)
// 본체 로직은 아직 다음 단계. 지금은 수신 즉시 ACK만 회신하는 최소 동작.

void FSM_HandleMove(const MoveCmd_t *cmd)
{
    // TODO: g_persist_state.move_state로 중복/충돌 판정,
    //       EEPROM_SaveState로 ACCEPTED 기록 후 ACK, 이후 MOVING/READY 전이
    Send_ACK(cmd->req_id, "MOVE");
	Send_WAIT(cmd->req_id);
}

void FSM_HandleDispense(const DispenseCmd_t *cmd)
{
    // TODO: g_persist_state.move_state == READY && 좌표 일치 확인,
    //       EEPROM_SaveState로 ACCEPTED 기록 후 ACK, 이후 DISPENSING 전이
    Send_ACK(cmd->req_id, "DISPENSE");
	Send_RESULT(cmd->req_id, cmd->x,  cmd->y, 1);
}

void FSM_HandleResultAck(uint32_t req_id)
{
    // TODO: g_persist_state.dispense_state를 RESULT_ACKED로 전이하고 EEPROM_SaveState
    (void)req_id;
}
