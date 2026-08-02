/*
 * TEST.UART.c
 *
 * Created: 2026-08-02 오전 9:51:41
 * Author : kccistc
 */ 
#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>

#define BAUD 9600
#define UBRR_VAL ((F_CPU/16/BAUD)-1)

volatile uint8_t rx_data;
volatile uint8_t rx_flag = 0;


void UART0_init(void) {
	UBRR0H = (uint8_t)(UBRR_VAL >> 8);
	UBRR0L = (uint8_t)UBRR_VAL;

	// RX 인터럽트, RX/TX 활성화
	UCSR0B = (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);

	// 8N1
	UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

void UART0_transmit(uint8_t data) {
	while(!(UCSR0A & (1 << UDRE0)));
	UDR0 = data;
}

void UART0_sendString(const char *str) {
	while (*str) UART0_transmit(*str++);
}

ISR(USART0_RX_vect) {
	rx_data = UDR0;   // UDR0 읽는 순간 RXC0 플래그 자동 클리어
	rx_flag = 1;
}

int main(void) {
	UART0_init();
	sei();

	while (1) {
		if (rx_flag) {
			rx_flag = 0;

			UART0_sendString("ACK");  // 수신 완료 메시지 회신
		}
	}
}

