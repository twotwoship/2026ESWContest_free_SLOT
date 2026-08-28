#ifndef UART_PROTOCOL_H
#define UART_PROTOCOL_H

#include <stdint.h>

// ==================== 설정값 ====================
#define RINGBUF_SIZE     128    // 2의 거듭제곱 권장 (마스킹 최적화)
#define MAX_FRAME_LEN    64     // 명세서 4번: 최대 프레임 64 bytes
#define MAX_FIELDS       6
#define MAX_FIELD_LEN    16

// ==================== 링버퍼 ====================
void RingBuf_Init(void);
void RingBuf_Push(uint8_t byte);          // ISR 전용
uint8_t RingBuf_Pop(uint8_t *out);        // 메인 루프 전용, 성공 시 1
uint8_t RingBuf_IsEmpty(void);

// ==================== UART 저수준 ====================
void UART0_Init(unsigned long baud);
void UART0_TxChar(char data);
void UART0_TxString(const char *str);

// ==================== 프레임 추출 ====================
// 링버퍼에서 바이트를 꺼내며 LF(0x0A) 기준으로 한 프레임 완성
// CR(0x0D)은 만나면 버림. max_len 초과 시 프레임 폐기.
// 반환: 1 = 완성된 프레임 있음(frame_out에 저장), 0 = 아직 미완성
uint8_t Frame_TryExtract(char *frame_out, uint8_t max_len);

// 64바이트 초과로 프레임이 폐기된 적이 있으면 1을 반환하고 플래그를 소비한다.
uint8_t Frame_ConsumeOverflowFlag(void);

// ==================== 문자열/숫자 유틸 ====================
uint8_t Split_Fields(const char *frame, char fields[][MAX_FIELD_LEN], uint8_t max_fields);
uint8_t IsAllDigits(const char *s);
uint8_t IsAllHexDigits(const char *s);
uint32_t HexToUint32(const char *s);
uint32_t DecToUint32(const char *s);
void Uint32ToHex8(uint32_t val, char *out);

// ==================== 명령 타입 ====================
typedef enum { CMD_MOVE, CMD_DISPENSE, CMD_ACK, CMD_UNKNOWN } CmdType_t;
CmdType_t Parse_CmdType(const char *field0);

// ==================== 명령별 파서 ====================
typedef struct {
    uint32_t req_id;
    uint8_t  x;
    uint8_t  y;
    uint32_t allowed_sec;
} MoveCmd_t;

typedef struct {
    uint32_t req_id;
    uint8_t  x;
    uint8_t  y;
} DispenseCmd_t;

// 반환값: 0=성공, 1=INVALID_FORMAT, 2=INVALID_COORD, 3=INVALID_TIME
uint8_t Parse_MOVE(const char *frame, MoveCmd_t *out);
uint8_t Parse_DISPENSE(const char *frame, DispenseCmd_t *out);
uint8_t Parse_ACK_RESULT(const char *frame, uint32_t *req_id);

// ==================== 응답 송신 ====================
void Send_ACK(uint32_t req_id, const char *cmd_name);
void Send_WAIT(uint32_t req_id);
void Send_RESULT(uint32_t req_id, uint8_t x, uint8_t y, uint8_t r);
void Send_ERROR(uint32_t req_id, const char *error_code);

// ==================== 통합 처리 태스크 ====================
// 링버퍼 -> 프레임 추출 -> 파싱 -> FSM 디스패치까지 한번에 처리
// 메인 루프에서 매 사이클 호출
void Task_UartRxProcess(void);

// ==================== FSM 핸들러 (fsm.c에서 구현) ====================
// 파서까지 통과한 유효 명령을 실제 상태머신으로 넘기는 지점
void FSM_HandleMove(const MoveCmd_t *cmd);
void FSM_HandleDispense(const DispenseCmd_t *cmd);
void FSM_HandleResultAck(uint32_t req_id);

#endif // UART_PROTOCOL_H
