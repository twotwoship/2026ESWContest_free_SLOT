#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include "types.h"

bool Protocol_Parse(const char *line, Frame *out);

uint8_t Protocol_ParseHex32(const char *s, uint32_t *out);

void Protocol_SendAck(uint32_t req_id, const char *target);   /* ACK|req|MOVE 등 (MOVE/DISPENSE/TIMEOUT/RST) */
void Protocol_SendWait(uint32_t req_id);                      /* WAIT|req */
void Protocol_SendResult(uint32_t req_id, uint8_t x, uint8_t y, DispenseResult r);
                                                                /* RESULT|req|XYR */
void Protocol_SendError(uint32_t req_id, ErrorCode code);      /* ERROR|req|CODE */
void Protocol_SendInvalidFormat(void);                         /* ERROR|INVALID_FORMAT (2필드, 요청번호 없음) */
const char *Protocol_ErrorText(ErrorCode code);                /* ErrorCode -> 문자열 */

#endif
