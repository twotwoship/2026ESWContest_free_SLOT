/*
 * sg255_test.c
 * ATmega128A - Kodenshi SG-255 투과형 포토인터럽터 테스트 코드
 *
 * 핀맵:
 *   PD0 (INT0) - 홈 센서 X축 (SG255 #1 OUT)
 *   PD1 (INT1) - 홈 센서 Y축 (SG255 #2 OUT)
 *
 * SG255 회로:
 *   LED측: VCC --[220ohm]-- Anode | Cathode -- GND
 *   TR측:  VCC --[4.7kohm]-- Collector(=OUT, MCU 핀 연결)
 *                              Emitter -- GND
 *
 * 극성:
 *   빔 클리어 -> 포토TR 도통 -> OUT = LOW
 *   빔 차단   -> 포토TR 차단 -> OUT = HIGH
 *
 * => 감지 이벤트 = Rising Edge (LOW -> HIGH)
 *
 * UART0: 9600bps, 8N1
 */

#define F_CPU 16000000UL

#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdio.h>

#define BAUD 9600
#define UBRR_VALUE ((F_CPU / (16UL * BAUD)) - 1)

/* ---------------- 홈 센서 핀 정의 ---------------- */

/* X축 홈 센서 */
#define IR1_PORT  PIND
#define IR1_PIN   PD0

/* Y축 홈 센서 */
#define IR2_PORT  PIND
#define IR2_PIN   PD1

/* SG255 극성: 감지(차단) = HIGH */
#define IR_BLOCKED_STATE 1

volatile uint8_t ir1_isr_flag = 0;
volatile uint8_t ir2_isr_flag = 0;

static uint8_t ir1_prev_state = 0;
static uint8_t ir2_prev_state = 0;


/* ---------------- UART0 ---------------- */

static void uart0_init(void)
{
    UBRR0H = (uint8_t)(UBRR_VALUE >> 8);
    UBRR0L = (uint8_t)(UBRR_VALUE & 0xFF);

    UCSR0B = (1 << TXEN0);

    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

static void uart0_putc(char c)
{
    while (!(UCSR0A & (1 << UDRE0)));

    UDR0 = c;
}

static void uart0_puts(const char *s)
{
    while (*s) {
        uart0_putc(*s++);
    }
}


/* ---------------- IR 센서 초기화 ---------------- */

static void ir_sensors_init(void)
{
    /*
     * PD0 = INT0 = X축 홈 센서
     * PD1 = INT1 = Y축 홈 센서
     *
     * 외부 풀업 저항을 사용하므로
     * 내부 풀업은 사용하지 않음.
     */

    DDRD &= ~((1 << PD0) | (1 << PD1));

    PORTD &= ~((1 << PD0) | (1 << PD1));


    /*
     * EICRA
     *
     * INT0:
     * ISC01 = 0
     * ISC00 = 1
     * -> Any Logical Change
     *
     * INT1:
     * ISC11 = 0
     * ISC10 = 1
     * -> Any Logical Change
     */

    EICRA &= ~((1 << ISC01) | (1 << ISC11));

    EICRA |= (1 << ISC00) | (1 << ISC10);


    /* INT0, INT1 인터럽트 활성화 */

    EIMSK |= (1 << INT0) | (1 << INT1);

    sei();
}


/* ---------------- ISR ---------------- */

ISR(INT0_vect)
{
    ir1_isr_flag = 1;
}

ISR(INT1_vect)
{
    ir2_isr_flag = 1;
}


/* ---------------- 메인 ---------------- */

int main(void)
{
    uart0_init();
    ir_sensors_init();

    uart0_puts("SG255 HOME sensor test start\r\n");
    uart0_puts("X = PD0(INT0), Y = PD1(INT1)\r\n");
    uart0_puts("polarity: BLOCKED = HIGH\r\n");


    while (1)
    {
        /* ---------------- X축 홈 센서 ---------------- */

        if (ir1_isr_flag)
        {
            ir1_isr_flag = 0;

            uint8_t cur =
                (IR1_PORT & (1 << IR1_PIN)) ? 1 : 0;


            if (ir1_prev_state != IR_BLOCKED_STATE &&
                cur == IR_BLOCKED_STATE)
            {
                uart0_puts(
                    "X HOME: BLOCKED (rising edge)\r\n"
                );
            }
            else if (ir1_prev_state == IR_BLOCKED_STATE &&
                     cur != IR_BLOCKED_STATE)
            {
                uart0_puts(
                    "X HOME: CLEARED (falling edge)\r\n"
                );
            }

            ir1_prev_state = cur;
        }


        /* ---------------- Y축 홈 센서 ---------------- */

        if (ir2_isr_flag)
        {
            ir2_isr_flag = 0;

            uint8_t cur =
                (IR2_PORT & (1 << IR2_PIN)) ? 1 : 0;


            if (ir2_prev_state != IR_BLOCKED_STATE &&
                cur == IR_BLOCKED_STATE)
            {
                uart0_puts(
                    "Y HOME: BLOCKED (rising edge)\r\n"
                );
            }
            else if (ir2_prev_state == IR_BLOCKED_STATE &&
                     cur != IR_BLOCKED_STATE)
            {
                uart0_puts(
                    "Y HOME: CLEARED (falling edge)\r\n"
                );
            }

            ir2_prev_state = cur;
        }
    }

    return 0;
}