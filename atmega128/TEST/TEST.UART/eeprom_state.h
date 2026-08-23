#ifndef EEPROM_STATE_H
#define EEPROM_STATE_H

#include <stdint.h>

// ==================== 상태 코드 ====================
// 명세서 6.1: move_state / dispense_state
typedef enum {
	MOVE_STATE_NONE     = 0,
	MOVE_STATE_ACCEPTED = 1,
	MOVE_STATE_MOVING   = 2,
	MOVE_STATE_READY    = 3
} MoveState_t;

typedef enum {
	DISPENSE_STATE_NONE          = 0,
	DISPENSE_STATE_ACCEPTED      = 1,
	DISPENSE_STATE_DISPENSING    = 2,
	DISPENSE_STATE_RESULT_READY  = 3,
	DISPENSE_STATE_RESULT_ACKED  = 4
} DispenseState_t;

// ==================== 저장 레코드 ====================
// 명세서 6.1 항목 그대로 반영. CRC는 이 구조체 중 crc 필드를 제외한
// 전체에 대해 계산한다 (레이아웃 변경 시 EEPROM_MAGIC_VERSION을 올려서
// 이전 레코드를 자동으로 무효화할 것).
#define EEPROM_MAGIC_VERSION  0xA511   // 레이아웃 바뀌면 값 변경

typedef struct {
	uint16_t magic_version;

	uint32_t last_move_request_id;
	uint8_t  last_move_x;
	uint8_t  last_move_y;
	uint32_t allowed_seconds;
	uint8_t  move_state;            // MoveState_t

	uint32_t last_dispense_request_id;
	uint8_t  last_dispense_x;
	uint8_t  last_dispense_y;
	uint8_t  dispense_state;        // DispenseState_t

	uint8_t  last_result_xyr;       // 상위 니블: 사용 안 함 / X,Y,R을 각각 바이트로 저장하려면 구조 확장 가능
	uint8_t  last_result_x;
	uint8_t  last_result_y;
	uint8_t  last_result_r;

	uint16_t record_crc;            // 반드시 마지막 필드 - CRC 계산 대상에서 제외됨
} PersistState_t;

// ==================== CRC16 ====================
// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) - 표준적이고 AVR에 가벼움
uint16_t CRC16_Calc(const uint8_t *data, uint16_t len);

// ==================== 저장/로드 ====================
// 저장: record_crc를 자동 계산해서 채운 뒤 EEPROM에 기록한다.
void EEPROM_SaveState(PersistState_t *st);

// 로드: EEPROM에서 읽고 magic_version과 CRC를 검증한다.
// 반환 1: 유효한 레코드 (st에 채워짐)
// 반환 0: 무효(최초 부팅, 레이아웃 변경, CRC 불일치 등) - st는 기본값으로 초기화됨
uint8_t EEPROM_LoadState(PersistState_t *st);

// 기본값(전부 NONE 상태)으로 초기화만 하고 EEPROM에는 쓰지 않는다.
void PersistState_SetDefault(PersistState_t *st);

// ==================== 부팅 시 복구 판정 ====================
// 명세서 6.3: MOVING/DISPENSING 도중 리셋된 경우 RECOVERY_REQUIRED를 반환해야 함
// 반환 1: 복구 필요 (해당 요청번호를 req_id_out에 채움, ATmega는 이 요청번호로
//         ERROR|req_id|RECOVERY_REQUIRED를 전송해야 함 - Pi 연결 전이면 호출부에서 보류 가능)
// 반환 0: 복구 불필요 (정상 재개 가능한 상태이거나 애초에 진행 중이던 요청이 없음)
uint8_t Recovery_CheckOnBoot(const PersistState_t *st, uint32_t *req_id_out);

#endif // EEPROM_STATE_H
