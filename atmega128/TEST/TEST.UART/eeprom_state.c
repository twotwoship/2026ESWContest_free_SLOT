#include <avr/eeprom.h>
#include <string.h>
#include "eeprom_state.h"

// EEPROM 내 고정 주소 (0번지 고정 - 다른 용도로 EEPROM을 쓰게 되면
// 여기 오프셋을 옮기고 겹치지 않게 관리할 것)
#define EEPROM_STATE_ADDR   ((void *)0x0000)

// ==================== CRC16 (CCITT-FALSE, poly 0x1021, init 0xFFFF) ====================
uint16_t CRC16_Calc(const uint8_t *data, uint16_t len)
{
	uint16_t crc = 0xFFFF;

	for (uint16_t i = 0; i < len; i++)
	{
		crc ^= (uint16_t)data[i] << 8;

		for (uint8_t bit = 0; bit < 8; bit++)
		{
			if (crc & 0x8000)
			{
				crc = (uint16_t)((crc << 1) ^ 0x1021);
			}
			else
			{
				crc = (uint16_t)(crc << 1);
			}
		}
	}

	return crc;
}

// ==================== 기본값 ====================
void PersistState_SetDefault(PersistState_t *st)
{
	memset(st, 0, sizeof(PersistState_t));
	st->magic_version = EEPROM_MAGIC_VERSION;
	st->move_state = MOVE_STATE_NONE;
	st->dispense_state = DISPENSE_STATE_NONE;
	// record_crc는 EEPROM_SaveState에서 채워짐
}

// ==================== 저장 ====================
void EEPROM_SaveState(PersistState_t *st)
{
	st->magic_version = EEPROM_MAGIC_VERSION;

	// record_crc 필드를 제외한 전체 구조체에 대해 CRC 계산
	uint16_t crc_len = (uint16_t)(sizeof(PersistState_t) - sizeof(st->record_crc));
	st->record_crc = CRC16_Calc((const uint8_t *)st, crc_len);

	// eeprom_update_block: 이미 같은 값이면 실제 쓰기(마모)를 건너뛰므로
	// eeprom_write_block보다 EEPROM 수명에 유리함
	eeprom_update_block((const void *)st, EEPROM_STATE_ADDR, sizeof(PersistState_t));
}

// ==================== 로드 ====================
uint8_t EEPROM_LoadState(PersistState_t *st)
{
	eeprom_read_block((void *)st, EEPROM_STATE_ADDR, sizeof(PersistState_t));

	if (st->magic_version != EEPROM_MAGIC_VERSION)
	{
		// 최초 부팅이거나 레이아웃이 바뀐 경우 - 무효 처리
		PersistState_SetDefault(st);
		return 0;
	}

	uint16_t crc_len = (uint16_t)(sizeof(PersistState_t) - sizeof(st->record_crc));
	uint16_t calc_crc = CRC16_Calc((const uint8_t *)st, crc_len);

	if (calc_crc != st->record_crc)
	{
		// CRC 불일치 - EEPROM 손상 가능성. 안전하게 기본값으로 되돌림
		// (명세서 9번 25항: CRC 검증 실패 시 모터 금지와 일맥상통 -
		//  여기서는 아예 진행 중 상태 자체를 신뢰하지 않는 쪽으로 처리)
		PersistState_SetDefault(st);
		return 0;
	}

	return 1;
}

// ==================== 부팅 복구 판정 ====================
uint8_t Recovery_CheckOnBoot(const PersistState_t *st, uint32_t *req_id_out)
{
	// 명세서 6.3: MOVING 또는 DISPENSING 도중 리셋된 경우에만 RECOVERY_REQUIRED
	// READY / RESULT_READY는 정상 재개 가능한 상태이므로 복구 대상 아님

	if (st->move_state == MOVE_STATE_MOVING)
	{
		*req_id_out = st->last_move_request_id;
		return 1;
	}

	if (st->dispense_state == DISPENSE_STATE_DISPENSING)
	{
		*req_id_out = st->last_dispense_request_id;
		return 1;
	}

	return 0;
}
