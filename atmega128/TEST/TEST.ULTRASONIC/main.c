/*
 * ATmega128A 초음파(HC-SR04) 거리 측정 + UART 출력
 * ------------------------------------------------
 * TRIG : PD4 (출력)
 * ECHO : PD2 (INT2, 외부 인터럽트로 상승/하강 엣지 캡처)
 *
 * 측정 원리
 * 1) TRIG에 10us HIGH 펄스 인가 -> 센서가 40kHz 초음파 8펄스 발사
 * 2) 센서가 반사파를 받으면 ECHO 핀을 HIGH로 올리고, 반사파 수신 시 LOW로 내림
 *    -> ECHO가 HIGH인 시간(pulse width)이 왕복 시간
 * 3) INT2 엣지 인터럽트에서 상승엣지 때 TCNT1=0으로 리셋,
 *    하강엣지 때 TCNT1 값을 캡처하여 pulse width(타이머 틱 수)를 구함
 * 4) 거리(cm) = 시간(us) / 58   (음속 340m/s 기준 왕복 보정)
 *
 * Timer1: 분주비 8, F_CPU=16MHz -> 1틱 = 0.5us, 최대 32.768ms 측정 가능
 *         (HC-SR04 최대 유효거리 4m 왕복 약 23ms 이므로 충분)
 */

#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------- UART0 (9600bps, 8N1) ------------------- */
#define BAUD 9600
#define UBRR_VALUE ((F_CPU / 16 / BAUD) - 1)

void UART0_init(void)
{
    UBRR0H = (unsigned char)(UBRR_VALUE >> 8);
    UBRR0L = (unsigned char)UBRR_VALUE;
    UCSR0B = (1 << RXEN0) | (1 << TXEN0);      // 송수신 Enable 
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);    // 8N1 
}

void UART0_transmit(unsigned char data)
{
    while (!(UCSR0A & (1 << UDRE0)));  // 송신 버퍼 비었는지 대기
    UDR0 = data;
}

void UART0_print(const char *str)
{
    while (*str) {
        UART0_transmit((unsigned char)*str++);
    }
}

/* ------------------- 초음파(ECHO) 캡처 관련 전역변수 ------------------- */
volatile uint8_t  echo_edge_state = 0;   // 0: 상승엣지 대기, 1: 하강엣지 대기
volatile uint16_t echo_ticks      = 0;   // 캡처된 타이머 틱 값 
volatile uint8_t  echo_done       = 0;   // 측정 완료 플래그 
volatile uint8_t  timer1_ovf      = 0;   // Timer1 오버플로우(타임아웃) 플래그 

/* ------------------- INT2 (PD2, ECHO) 초기화 ------------------- */
void ECHO_INT2_init(void)
{
    DDRD  &= ~(1 << PD2);   // PD2 입력 (ECHO) 

    /* EICRA: INT2 sense control (ISC21:ISC20)
     * 11 = 상승엣지, 10 = 하강엣지 */
    EICRA |= (1 << ISC21) | (1 << ISC20);  // 처음엔 상승엣지 대기

    EIMSK |= (1 << INT2);   // INT2 인터럽트 Enable
}

/* ------------------- TRIG (PD4) 초기화 ------------------- */
void TRIG_init(void)
{
    DDRD  |= (1 << PD4);    // PD4 출력 (TRIG)
    PORTD &= ~(1 << PD4);   // 초기 LOW 
}

/* ------------------- Timer1 초기화 (거리 측정용 자유 카운터) ------------------- */
void Timer1_init(void)
{
    TCCR1A = 0x00;
    TCCR1B = (1 << CS11);   // 분주비 8 -> 0.5us/tick @16MHz 
    TIMSK  |= (1 << TOIE1); // Timer1 오버플로우 인터럽트 Enable (타임아웃 검출용)
}

/* ------------------- ISR: ECHO 엣지 캡처 ------------------- */
ISR(INT2_vect)
{
    if (echo_edge_state == 0) {
        // 상승엣지 감지: 타이머 리셋 후 하강엣지 대기로 전환
        TCNT1 = 0;
        timer1_ovf = 0;
        echo_edge_state = 1;
        EICRA = (EICRA & ~(1 << ISC20)) | (1 << ISC21); // 10: 하강엣지
    } else {
        // 하강엣지 감지: 펄스폭(타이머 값) 캡처
        echo_ticks = TCNT1;
        echo_done  = 1;
        echo_edge_state = 0;
        EICRA |= (1 << ISC21) | (1 << ISC20);            // 11: 상승엣지로 복귀
    }
}

/* ------------------- ISR: Timer1 오버플로우 (타임아웃/무반사) ------------------- */
ISR(TIMER1_OVF_vect)
{
    timer1_ovf = 1;
}

/* ------------------- TRIG 펄스 발생 (10us) ------------------- */
void trigger_pulse(void)
{
    PORTD |= (1 << PD4);
    _delay_us(10);
    PORTD &= ~(1 << PD4);
}

int main(void)
{
    char buf[40];
    uint32_t time_us;
    uint32_t distance_cm;

    UART0_init();
    TRIG_init();
    ECHO_INT2_init();
    Timer1_init();
    sei();

    UART0_print("Ultrasonic Distance Test Start\r\n");

    while (1) {
        echo_done  = 0;
        timer1_ovf = 0;
        echo_edge_state = 0;
        EICRA |= (1 << ISC21) | (1 << ISC20);  

        trigger_pulse();

        while (!echo_done && !timer1_ovf) {
            ; 
        }

        if (echo_done) {
            /* 거리(cm) 계산: time_us = ticks * 0.5us, distance = time_us/58 */
            time_us     = (uint32_t)echo_ticks / 2;   /* 0.5us 단위 -> us 환산 */
            distance_cm = time_us / 58;

            sprintf(buf, "Distance: %lu cm (%lu us)\r\n", distance_cm, time_us);
            UART0_print(buf);
        } else {
            UART0_print("Distance: Out of range (timeout)\r\n");
        }

        _delay_ms(60);  /* HC-SR04 권장 트리거 간격(60ms 이상) 확보 */
    }

    return 0;
}