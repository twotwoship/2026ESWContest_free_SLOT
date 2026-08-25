#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include "uart_protocol.h"
#include "eeprom_state.h"

// 부팅 시 로드된 영속 상태 - fsm.c에서도 참조할 수 있도록 전역으로 둔다
PersistState_t g_persist_state;

int main(void)
{
	UART0_Init(9600);
	sei();

	// 부팅 시 EEPROM 상태 로드 (명세서 6.3, 10번 AT-14)
	uint8_t loaded = EEPROM_LoadState(&g_persist_state);

	if (loaded)
	{
		uint32_t recovery_req_id;
		if (Recovery_CheckOnBoot(&g_persist_state, &recovery_req_id))
		{
			// MOVING/DISPENSING 도중 리셋됨 - 자동 재구동 금지, RECOVERY_REQUIRED 통보
			Send_ERROR(recovery_req_id, "RECOVERY_REQUIRED");
		}
	}
	// loaded == 0: 최초 부팅이거나 CRC 무효 - g_persist_state는 이미 기본값(NONE)으로 세팅됨

	UART0_TxString("SLOT-GUARD ATmega128A Ready\r\n");

	while (1)
	{
		Task_UartRxProcess();   // 링버퍼 -> 프레임 추출 -> 파싱 -> FSM 디스패치

		// Task_MotorPoll();  // 모터/서보 진행 상태 폴링 (다음 단계)
	}
}
