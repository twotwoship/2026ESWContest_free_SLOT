#include <string.h>
#include "uart_protocol.h"

// ==================== 프레임 추출 ====================
// 여러 번 호출해도 안전하도록 진행 상태를 static으로 보관
static char    frame_wip[MAX_FRAME_LEN + 1];
static uint8_t frame_wip_len = 0;
static uint8_t frame_discarding = 0;      // 64바이트 초과로 현재 폐기 중인 프레임인지
static uint8_t frame_overflow_latch = 0;  // 폐기가 확정되어 아직 보고되지 않은 이벤트

// 링버퍼에 쌓인 바이트를 한 개씩 소비하며 LF를 만나면 완성된 프레임을 반환한다.
// 반환 1: frame_out에 완성된 프레임 문자열이 채워짐
// 반환 0: 링버퍼가 비었고 아직 완성된 프레임 없음
uint8_t Frame_TryExtract(char *frame_out, uint8_t max_len)
{
	uint8_t byte;

	while (RingBuf_Pop(&byte))
	{
		if (byte == '\n')
		{
			// 폐기 중이던 프레임의 끝이면 상태만 리셋하고 다음 프레임 탐색 계속
			if (frame_discarding)
			{
				frame_discarding = 0;
				frame_wip_len = 0;
				continue;
			}

			// 빈 프레임(연속 개행 등)이면 무시하고 다음 프레임 탐색 계속
			if (frame_wip_len == 0)
			{
				continue;
			}

			// 완성된 프레임을 출력 버퍼로 복사하고 즉시 반환
			uint8_t copy_len = frame_wip_len;
			if (copy_len > max_len) copy_len = max_len;
			memcpy(frame_out, frame_wip, copy_len);
			frame_out[copy_len] = '\0';

			frame_wip_len = 0;
			return 1;
		}
		else if (byte == '\r')
		{
			continue;   // CR은 허용 후 제거 (명세서 2번)
		}
		else
		{
			if (frame_discarding)
			{
				continue;   // 이미 폐기 확정된 프레임 - 계속 버림
			}

			if (frame_wip_len >= MAX_FRAME_LEN)
			{
				// 64바이트 초과 - 이 프레임은 폐기 확정, LF까지 나머지는 버림
				frame_discarding = 1;
				frame_overflow_latch = 1;
				frame_wip_len = 0;
				continue;
			}

			frame_wip[frame_wip_len++] = (char)byte;
		}
	}

	return 0;   // 링버퍼 소진, 완성된 프레임 없음
}

// 폐기된 프레임(오버플로) 이벤트가 있었는지 확인하고 소비(1회성)한다.
uint8_t Frame_ConsumeOverflowFlag(void)
{
	if (frame_overflow_latch)
	{
		frame_overflow_latch = 0;
		return 1;
	}
	return 0;
}

// ==================== 문자열/숫자 유틸 ====================
uint8_t Split_Fields(const char *frame, char fields[][MAX_FIELD_LEN], uint8_t max_fields)
{
	uint8_t field_idx = 0;
	uint8_t char_idx = 0;
	const char *p = frame;

	while (*p && field_idx < max_fields)
	{
		if (*p == '|')
		{
			fields[field_idx][char_idx] = '\0';
			field_idx++;
			char_idx = 0;
			p++;
			continue;
		}

		if (char_idx < MAX_FIELD_LEN - 1)
		{
			fields[field_idx][char_idx++] = *p;
		}
		p++;
	}

	if (field_idx < max_fields)
	{
		fields[field_idx][char_idx] = '\0';
		field_idx++;
	}

	return field_idx;
}

uint8_t IsAllDigits(const char *s)
{
	if (*s == '\0') return 0;
	while (*s)
	{
		if (*s < '0' || *s > '9') return 0;
		s++;
	}
	return 1;
}

uint8_t IsAllHexDigits(const char *s)
{
	if (*s == '\0') return 0;
	while (*s)
	{
		uint8_t is_digit = (*s >= '0' && *s <= '9');
		uint8_t is_upper_hex = (*s >= 'A' && *s <= 'F');
		if (!is_digit && !is_upper_hex) return 0;
		s++;
	}
	return 1;
}

uint32_t HexToUint32(const char *s)
{
	uint32_t val = 0;
	while (*s)
	{
		val <<= 4;
		if (*s >= '0' && *s <= '9') val |= (uint32_t)(*s - '0');
		else if (*s >= 'A' && *s <= 'F') val |= (uint32_t)(*s - 'A' + 10);
		s++;
	}
	return val;
}

uint32_t DecToUint32(const char *s)
{
	uint32_t val = 0;
	while (*s)
	{
		val = val * 10 + (uint32_t)(*s - '0');
		s++;
	}
	return val;
}

void Uint32ToHex8(uint32_t val, char *out)
{
	const char hex_chars[] = "0123456789ABCDEF";
	for (int8_t i = 7; i >= 0; i--)
	{
		out[i] = hex_chars[val & 0xF];
		val >>= 4;
	}
	out[8] = '\0';
}

// ==================== 명령 타입 판별 ====================
CmdType_t Parse_CmdType(const char *field0)
{
	if (strcmp(field0, "MOVE") == 0)     return CMD_MOVE;
	if (strcmp(field0, "DISPENSE") == 0) return CMD_DISPENSE;
	if (strcmp(field0, "ACK") == 0)      return CMD_ACK;
	return CMD_UNKNOWN;
}

// ==================== MOVE 파서 ====================
uint8_t Parse_MOVE(const char *frame, MoveCmd_t *out)
{
	char fields[MAX_FIELDS][MAX_FIELD_LEN];
	uint8_t n = Split_Fields(frame, fields, MAX_FIELDS);

	if (n != 5) return 1;
	if (strlen(fields[1]) != 8 || !IsAllHexDigits(fields[1])) return 1;
	if (strlen(fields[2]) != 1 || strlen(fields[3]) != 1) return 1;
	if (!IsAllDigits(fields[2]) || !IsAllDigits(fields[3])) return 1;
	if (strlen(fields[4]) != 6 || !IsAllDigits(fields[4])) return 1;

	out->req_id = HexToUint32(fields[1]);

	uint8_t x = (uint8_t)(fields[2][0] - '0');
	uint8_t y = (uint8_t)(fields[3][0] - '0');
	if (x > 1 || y > 4) return 2;

	uint32_t sec = DecToUint32(fields[4]);
	if (sec < 1 || sec > 999999) return 3;

	out->x = x;
	out->y = y;
	out->allowed_sec = sec;
	return 0;
}

// ==================== DISPENSE 파서 ====================
uint8_t Parse_DISPENSE(const char *frame, DispenseCmd_t *out)
{
	char fields[MAX_FIELDS][MAX_FIELD_LEN];
	uint8_t n = Split_Fields(frame, fields, MAX_FIELDS);

	if (n != 4) return 1;
	if (strlen(fields[1]) != 8 || !IsAllHexDigits(fields[1])) return 1;
	if (strlen(fields[2]) != 1 || strlen(fields[3]) != 1) return 1;
	if (!IsAllDigits(fields[2]) || !IsAllDigits(fields[3])) return 1;

	out->req_id = HexToUint32(fields[1]);

	uint8_t x = (uint8_t)(fields[2][0] - '0');
	uint8_t y = (uint8_t)(fields[3][0] - '0');
	if (x > 1 || y > 4) return 2;

	out->x = x;
	out->y = y;
	return 0;
}

// ==================== ACK(RESULT용) 파서 ====================
uint8_t Parse_ACK_RESULT(const char *frame, uint32_t *req_id)
{
	char fields[MAX_FIELDS][MAX_FIELD_LEN];
	uint8_t n = Split_Fields(frame, fields, MAX_FIELDS);

	if (n != 3) return 1;
	if (strcmp(fields[0], "ACK") != 0) return 1;
	if (strlen(fields[1]) != 8 || !IsAllHexDigits(fields[1])) return 1;
	if (strcmp(fields[2], "RESULT") != 0) return 1;

	*req_id = HexToUint32(fields[1]);
	return 0;
}

// ==================== 응답 송신 ====================
void Send_ACK(uint32_t req_id, const char *cmd_name)
{
	char id_str[9];
	Uint32ToHex8(req_id, id_str);

	UART0_TxString("ACK|");
	UART0_TxString(id_str);
	UART0_TxChar('|');
	UART0_TxString(cmd_name);
	UART0_TxString("\n");
}

void Send_WAIT(uint32_t req_id)
{
	char id_str[9];
	Uint32ToHex8(req_id, id_str);
	UART0_TxString("WAIT|");
	UART0_TxString(id_str);
	UART0_TxString("\n");
}

void Send_RESULT(uint32_t req_id, uint8_t x, uint8_t y, uint8_t r)
{
	char id_str[9];
	Uint32ToHex8(req_id, id_str);
	UART0_TxString("RESULT|");
	UART0_TxString(id_str);
	UART0_TxChar('|');
	UART0_TxChar((char)('0' + x));
	UART0_TxChar((char)('0' + y));
	UART0_TxChar((char)('0' + r));
	UART0_TxString("\n");
}

void Send_ERROR(uint32_t req_id, const char *error_code)
{
	char id_str[9];
	Uint32ToHex8(req_id, id_str);
	UART0_TxString("ERROR|");
	UART0_TxString(id_str);
	UART0_TxChar('|');
	UART0_TxString(error_code);
	UART0_TxString("\n");
}

// ==================== 통합 처리 태스크 ====================
// 링버퍼 -> 프레임 추출 -> 파싱 -> FSM 디스패치
// 메인 루프에서 매 사이클 호출. 한 번 호출에 완성된 프레임을 모두 소진한다.
void Task_UartRxProcess(void)
{
	static char frame[MAX_FRAME_LEN + 1];

	while (Frame_TryExtract(frame, MAX_FRAME_LEN))
	{
		char fields[MAX_FIELDS][MAX_FIELD_LEN];
		uint8_t n = Split_Fields(frame, fields, MAX_FIELDS);

		if (n == 0)
		{
			continue;   // 빈 프레임, 무시
		}

		CmdType_t cmd = Parse_CmdType(fields[0]);

		switch (cmd)
		{
			case CMD_MOVE:
			{
				MoveCmd_t move;
				uint8_t err = Parse_MOVE(frame, &move);
				if (err == 0)
				{
					FSM_HandleMove(&move);
				}
				else
				{
					// req_id를 파싱하지 못했을 수 있으므로 필드에서 재시도
					uint32_t req_id = 0;
					if (strlen(fields[1]) == 8 && IsAllHexDigits(fields[1]))
					{
						req_id = HexToUint32(fields[1]);
					}
					const char *code = (err == 2) ? "INVALID_COORD"
					: (err == 3) ? "INVALID_TIME"
					: "INVALID_FORMAT";
					Send_ERROR(req_id, code);
				}
				break;
			}

			case CMD_DISPENSE:
			{
				DispenseCmd_t disp;
				uint8_t err = Parse_DISPENSE(frame, &disp);
				if (err == 0)
				{
					FSM_HandleDispense(&disp);
				}
				else
				{
					uint32_t req_id = 0;
					if (strlen(fields[1]) == 8 && IsAllHexDigits(fields[1]))
					{
						req_id = HexToUint32(fields[1]);
					}
					const char *code = (err == 2) ? "INVALID_COORD" : "INVALID_FORMAT";
					Send_ERROR(req_id, code);
				}
				break;
			}

			case CMD_ACK:
			{
				uint32_t req_id;
				if (Parse_ACK_RESULT(frame, &req_id) == 0)
				{
					FSM_HandleResultAck(req_id);
				}
				// ACK 프레임 자체가 깨졌으면 응답할 대상이 불명확하므로 무시
				break;
			}

			default:
			{
				// 요청번호 자리를 최대한 회수 시도 (명세상 두 번째 필드가 보통 ID)
				uint32_t req_id = 0;
				if (n >= 2 && strlen(fields[1]) == 8 && IsAllHexDigits(fields[1]))
				{
					req_id = HexToUint32(fields[1]);
				}
				Send_ERROR(req_id, "INVALID_FORMAT");
				break;
			}
		}
	}

	if (Frame_ConsumeOverflowFlag())
	{
		// 64바이트를 초과한 프레임은 요청번호를 알 수 없으므로 0으로 보고
		Send_ERROR(0, "INVALID_FORMAT");
	}
}
