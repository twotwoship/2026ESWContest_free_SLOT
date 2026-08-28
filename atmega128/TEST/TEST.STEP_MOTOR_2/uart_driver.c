#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include "uart_protocol.h"

// ==================== 링버퍼 내부 상태 ====================
static volatile uint8_t  rb_buf[RINGBUF_SIZE];
static volatile uint8_t  rb_head = 0;   // 쓰기 위치 (ISR)
static volatile uint8_t  rb_tail = 0;   // 읽기 위치 (메인 루프)

void RingBuf_Init(void)
{
    rb_head = 0;
    rb_tail = 0;
}

// ISR 전용 - 절대 blocking 하지 않음
void RingBuf_Push(uint8_t byte)
{
    uint8_t next = (uint8_t)((rb_head + 1) % RINGBUF_SIZE);
    if (next == rb_tail)
    {
        // 버퍼 풀 - 오버런. 정책상 가장 오래된 바이트를 버리지 않고
        // 새 바이트를 버림 (프레임 경계가 깨지는 것을 최소화하기 위함)
        return;
    }
    rb_buf[rb_head] = byte;
    rb_head = next;
}

// 메인 루프 전용
uint8_t RingBuf_IsEmpty(void)
{
    return (rb_head == rb_tail);
}

uint8_t RingBuf_Pop(uint8_t *out)
{
    if (RingBuf_IsEmpty()) return 0;

    *out = rb_buf[rb_tail];
    rb_tail = (uint8_t)((rb_tail + 1) % RINGBUF_SIZE);
    return 1;
}

// ==================== UART0 저수준 ====================
void UART0_Init(unsigned long baud)
{
    unsigned int ubrr = (unsigned int)((F_CPU / 16 / baud) - 1);
    UBRR0H = (uint8_t)(ubrr >> 8);
    UBRR0L = (uint8_t)ubrr;

    // RX 인터럽트, RX/TX 활성화
    UCSR0B = (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);
    // 8N1
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);

    RingBuf_Init();
}

void UART0_TxChar(char data)
{
    while (!(UCSR0A & (1 << UDRE0)));
    UDR0 = (uint8_t)data;
}

void UART0_TxString(const char *str)
{
    while (*str)
    {
        UART0_TxChar(*str++);
    }
}

// ISR은 링버퍼에 push만 함 - 파싱/처리는 절대 여기서 하지 않음
ISR(USART0_RX_vect)
{
    uint8_t data = UDR0;   // 읽는 순간 RXC0 플래그 자동 클리어
    RingBuf_Push(data);
}
