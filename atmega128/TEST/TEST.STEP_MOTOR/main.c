/*
 * 28BYJ-48 + ULN2003 스테퍼 모터 - UART 방향 제어
 * ATmega128A, F_CPU = 16MHz, UART0 9600bps
 *
 * 배선:
 *   PC0 -> ULN2003 IN1
 *   PC1 -> ULN2003 IN2
 *   PC2 -> ULN2003 IN3
 *   PC3 -> ULN2003 IN4
 *   ULN2003 VDD -> 외부 5V (CP2102 5V)
 *   ULN2003 GND -> 공통 GND
 *
 * UART 명령 (터미널에서 문자 입력):
 *   'f' : 정방향 회전 시작
 *   'b' : 역방향 회전 시작
 *   's' : 정지
 */
#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>

// 4상 풀스텝 시퀀스 (IN1~IN4 = PC0~PC3)
static const uint8_t FULL_STEP[4] = {
    0b0001,
    0b0010,
    0b0100,
    0b1000
};

#define STEP_DELAY_MS 3

// 모터 상태: 0 = 정지, 1 = 정방향, 2 = 역방향
volatile uint8_t motor_state = 0;

void motor_write(uint8_t pattern)
{
    PORTC = (PORTC & 0xF0) | (pattern & 0x0F);
}

void uart0_init(uint16_t baud)
{
    uint16_t ubrr = (F_CPU / 16 / baud) - 1;

    UBRR0H = (uint8_t)(ubrr >> 8);
    UBRR0L = (uint8_t)ubrr;

    // 송신, 수신, RX 인터럽트 모두 사용
    UCSR0B = (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);

    // 비동기, 패리티 없음, 스톱비트 1, 데이터 8비트
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

void uart0_putc(char c)
{
    while (!(UCSR0A & (1 << UDRE0)))
        ;
    UDR0 = c;
}

void uart0_puts(const char *s)
{
    while (*s) {
        uart0_putc(*s);
        s++;
    }
}

// UART 수신 인터럽트: 문자 하나 받을 때마다 모터 상태만 갱신
ISR(USART0_RX_vect)
{
    uint8_t cmd = UDR0;

    switch (cmd) {
        case 'f':
        case 'F':
            motor_state = 1;
            uart0_puts("FORWARD\r\n");
            break;
        case 'b':
        case 'B':
            motor_state = 2;
            uart0_puts("BACKWARD\r\n");
            break;
        case 's':
        case 'S':
            motor_state = 0;
            uart0_puts("STOP\r\n");
            break;
        default:
            // 정의되지 않은 명령은 무시
            break;
    }
}

int main(void)
{
    DDRC |= 0x0F;      // PC0~PC3 출력
    uart0_init(9600);

    sei();             // 전역 인터럽트 활성화

    static uint8_t step_index = 0;

    uart0_puts("READY (f/b/s)\r\n");

    while (1) {
        switch (motor_state) {
            case 1: // 정방향
                motor_write(FULL_STEP[step_index % 4]);
                step_index++;
                _delay_ms(STEP_DELAY_MS);
                break;

            case 2: // 역방향
                motor_write(FULL_STEP[step_index % 4]);
                // 부호 있는 뺄셈이 아니라 +3(=-1 mod 4)로 처리하여
                // uint8_t 언더플로우를 피함
                step_index = (step_index + 3) % 4;
                _delay_ms(STEP_DELAY_MS);
                break;

            case 0: // 정지
            default:
                // 아무것도 안 함 (다음 명령 대기)
                break;
        }
    }

    return 0;
}