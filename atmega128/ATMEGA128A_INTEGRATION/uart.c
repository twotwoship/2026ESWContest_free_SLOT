// =====================================================================
// uart.c
// =====================================================================
#include <avr/io.h>
#include <avr/interrupt.h>
#include <string.h>
#include "uart.h"
#include "config.h"

// 인덱스 계산에 & MASK를 사용 - 링버퍼 크기는 반드시 2의 거듭제곱이어야 함
#define RX_MASK (UART_RX_RING_SIZE - 1U)
#define TX_MASK (UART_TX_RING_SIZE - 1U)

static volatile uint8_t s_rx_buf[UART_RX_RING_SIZE];
static volatile uint8_t s_rx_head = 0;
static volatile uint8_t s_rx_tail = 0;
static volatile uint8_t s_tx_buf[UART_TX_RING_SIZE];
static volatile uint8_t s_tx_head = 0;
static volatile uint8_t s_tx_tail = 0;

// 현재 RX 링버퍼에 들어 있는 데이터 개수.
static uint8_t Uart_RxCount(void)
{
    return (uint8_t)((s_rx_head - s_rx_tail) & RX_MASK);
}

//현재 TX 링버퍼에 들어 있는 데이터 개수
static uint8_t Uart_TxCount(void)
{
    return (uint8_t)((s_tx_head - s_tx_tail) & TX_MASK);
}

//UART 초기화

void Uart_Init(void)
{
    uint16_t ubrr = (uint16_t)UART_UBRR_VALUE;

    s_rx_head = 0; //링버퍼 초기화
    s_rx_tail = 0;
    s_tx_head = 0;
    s_tx_tail = 0;

    UBRR0H = (uint8_t)(ubrr >> 8); //Baud Rate 설정
    UBRR0L = (uint8_t)(ubrr & 0xFFU);

    UCSR0A = 0x00;

    //8-bit data, No parity, 1 stop bit
    UCSR0C = (1U << UCSZ01) | (1U << UCSZ00);
    UCSR0B = (1U << RXEN0) | (1U << TXEN0) | (1U << RXCIE0);
}

// UART에서 1바이트가 수신될 때마다 호출
ISR(USART0_RX_vect)
{
    uint8_t data;
    uint8_t next;

    data = UDR0;				//반드시 UDR0을 읽어 수신 데이터를 가져옴

    next = (uint8_t)((s_rx_head + 1U) & RX_MASK);	//다음 head 위치 계산

    if (next != s_rx_tail) {			//공간이 있는 경우에만 저장
        s_rx_buf[s_rx_head] = data;
        s_rx_head = next;
    }
}


// UDR0이 비어 있고 UDRE 인터럽트가 활성화되어 있으면 호출
ISR(USART0_UDRE_vect)
{
    if (s_tx_tail != s_tx_head) {		//전송할 데이터가 있는 경우
        UDR0 = s_tx_buf[s_tx_tail];		//링버퍼에서 1바이트 꺼내 UART로 전송
        s_tx_tail = (uint8_t)((s_tx_tail + 1U) & TX_MASK);	//TX tail 이동
    } else {
        //전송할 데이터가 없으면 UDRE 인터럽트를 비활성화
        UCSR0B &= (uint8_t)~(1U << UDRIE0);
    }
}

//RX 링버퍼에서 LF가 포함된 한 줄 수신
bool Uart_ReadLine(char *dst, uint8_t cap)
{
    uint8_t count;
    uint8_t idx;
    uint8_t content_len;
    uint8_t copy_len;
    uint8_t i;

    if (dst == 0 || cap == 0U) return false; //출력 버퍼가 없거나 크기 0

    count = Uart_RxCount(); //현재 RX 데이터 개수 확인
    if (count == 0U) return false; //데이터가 하나도 없으면 바로 종료

    //LF 탐색 — 이 단계에서는 s_rx_tail을 변경하지 않음
    for (idx = 0U; idx < count; ++idx) {
        uint8_t pos = (uint8_t)((s_rx_tail + idx) & RX_MASK);
        if (s_rx_buf[pos] == FRAME_TERM_LF) break;
    }

    if (idx == count) {
        //LF가 없는 경우 - 현재 버퍼의 모든 데이터를 폐기하고 다음 수신 데이터부터 새로운 프레임으로 시작
        if (count == RX_MASK) {
            uint8_t sreg = SREG;

            //head가 ISR에서 변경되는 순간과 겹치지 않도록
            cli();
            s_rx_tail = s_rx_head;
            SREG = sreg;
        }
        return false;
    }

    content_len = idx;

    //CRLF 처리 — LF 바로 앞에 있는 CR만 제거
    if (content_len > 0U) {
        uint8_t last_pos = (uint8_t)((s_rx_tail + content_len - 1U) & RX_MASK);
        if (s_rx_buf[last_pos] == FRAME_TERM_CR) --content_len;
    }

    //복사할 데이터 길이 결정
    copy_len = content_len;
    if (copy_len >= cap) copy_len = (uint8_t)(cap - 1U);

    //문자열 복사
    for (i = 0U; i < copy_len; ++i) {
        uint8_t pos = (uint8_t)((s_rx_tail + i) & RX_MASK);
        dst[i] = (char)s_rx_buf[pos];
    }
    dst[copy_len] = '\0'; //문자열 NULL 종료

    //라인 전체 소비
    s_rx_tail = (uint8_t)((s_rx_tail + idx + 1U) & RX_MASK);

    return true;
}

//한 줄 송신 - 문자열을 TX 링버퍼에 저장하고 마지막에 LF를 추가
void Uart_WriteLine(const char *src)
{
    uint8_t sreg;
    size_t  raw_len;
    uint8_t len;
    uint8_t required;
    uint8_t free_space;
    uint8_t local_head;
    uint8_t i;

    if (src == 0) return; //NULL 방어

    raw_len = strlen(src);
    if (raw_len >= (size_t)UART_TX_RING_SIZE) return;
    len = (uint8_t)raw_len;

    required = (uint8_t)(len + 1U); //LF까지 포함한 실제 필요한 공간

    //TX 링버퍼의 현재 사용량을 기준으로 남은 공간 계산
    free_space = (uint8_t)(TX_MASK - Uart_TxCount());

    //프레임 전체가 들어갈 공간이 없으면 아무것도 저장하지 않음
    if (required > free_space) return;

    sreg = SREG; //현재 인터럽트 상태 저장
    cli();

    //현재 head를 로컬 변수에 복사
    local_head = s_tx_head;

    //문자열 저장
    for (i = 0U; i < len; ++i) {
        s_tx_buf[local_head] = (uint8_t)src[i];
        local_head = (uint8_t)((local_head + 1U) & TX_MASK);
    }

    //LF 저장
    s_tx_buf[local_head] = (uint8_t)FRAME_TERM_LF;
    local_head = (uint8_t)((local_head + 1U) & TX_MASK);

    s_tx_head = local_head; //TX head commit

    UCSR0B |= (1U << UDRIE0);

    SREG = sreg; //인터럽트 상태 복원
}

//아직 전송하지 않은 데이터가 TX 링버퍼에 있으면 true.
//true  -> TX 데이터 존재 / 전송 중 / false -> TX 링버퍼 비어 있음
bool Uart_TxBusy(void)
{
    return (s_tx_head != s_tx_tail);
}
