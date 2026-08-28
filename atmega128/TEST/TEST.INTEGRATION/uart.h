/* =====================================================================
 * uart.h — USART0 드라이버 + RX/TX 링버퍼
 * 9600-8-N-1, 인터럽트 기반, 논블로킹
 * ===================================================================== */
#ifndef UART_H
#define UART_H

#include <stdint.h>
#include <stdbool.h>

/* UBRR/RXEN/TXEN/RXCIE 설정, 링버퍼 초기화. sei() 는 호출하지 않는다. */
void Uart_Init(void);

/* LF 로 끝난 완성 라인 1개를 RX 링버퍼에서 꺼내 dst 에 담는다 (LF/CR 제거, NUL 종료).
 * dst 는 최대 cap-1 문자 + NUL. 완성된 라인이 없으면 즉시 false 를 반환하며 아무것도
 * 소비하지 않는다 (다음 호출에서 이어서 시도 가능). 라인이 cap-1 보다 길면 잘라서
 * 채우되 원본은 LF 까지 전부 소비한다 — 잘린 결과는 항상 FRAME_MAX_LEN 을 넘는
 * 길이가 되므로 protocol.c 의 길이 검사에서 자연히 INVALID_FORMAT 으로 걸러진다. */
bool Uart_ReadLine(char *dst, uint8_t cap);

/* src 문자열(NUL 종료, LF 미포함)을 TX 링버퍼에 넣고 끝에 LF 를 붙인다. 논블로킹 —
 * 링버퍼가 가득 차면 넘치는 뒷부분은 버려진다(설계상 TX_FRAME_BUF_LEN 이내로 호출). */
void Uart_WriteLine(const char *src);

/* TX 링버퍼에 아직 전송되지 않은 바이트가 남아있는지 여부 */
bool Uart_TxBusy(void);

#endif /* UART_H */
