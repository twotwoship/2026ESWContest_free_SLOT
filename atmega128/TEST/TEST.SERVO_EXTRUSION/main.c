/*
 * SG90 서보모터 UART 제어 테스트
 * ATmega128A
 *
 * 연결:
 *   - 서보 신호선(Signal) -> PB5 (OC1A, Timer1 PWM 출력)
 *   - 서보 VCC -> 5V (외부 전원 권장, 보드 5V로 여러 개 구동 시 전류 부족 주의)
 *   - 서보 GND -> ATmega128A GND와 공통 접지 필수
 *
 * UART로 문자 하나를 받으면 해당 각도로 회전:
 *   '0' -> 0도    (pulse 0.5ms)
 *   '1' -> 45도   (pulse 1.0ms)
 *   '2' -> 90도   (pulse 1.5ms)
 *   '3' -> 135도  (pulse 2.0ms)
 *   '4' -> 180도  (pulse 2.5ms)
 *
 * PWM: Timer1, Fast PWM (Mode 14, TOP=ICR1), 50Hz (20ms 주기)
 *      F_CPU = 16MHz, Prescaler = 8 -> 1 tick = 0.5us
 *      ICR1 = 40000 - 1  (20ms 주기)
 *      OCR1A = pulse_width(us) * 2
 *
 * UART: 9600bps, 8N1
 */

#define F_CPU 16000000UL
#include <avr/io.h>
#include <util/delay.h>

// ---------------- 서보 각도 -> Pulse Width(us) 매핑 ----------------
// 0.5ms(0도) ~ 2.5ms(180도), 45도 단위로 선형 매핑
#define PULSE_0DEG    500u    // 0.5ms
#define PULSE_45DEG   1000u   // 1.0ms
#define PULSE_90DEG   1500u   // 1.5ms
#define PULSE_135DEG  2000u   // 2.0ms
#define PULSE_180DEG  2500u   // 2.5ms

// ---------------- UART 함수 ----------------
void UART0_Init(unsigned long baud)
{
    unsigned int ubrr = (F_CPU / 16 / baud) - 1;

    UBRR0H = (unsigned char)(ubrr >> 8);
    UBRR0L = (unsigned char)ubrr;

    UCSR0B = (1 << TXEN0) | (1 << RXEN0);      // 송신, 수신 활성화
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);    // 8bit, 1 stop bit, no parity
}

void UART0_TxChar(char data)
{
    while (!(UCSR0A & (1 << UDRE0)));  // 송신 버퍼 비었는지 대기
    UDR0 = data;
}

void UART0_TxString(const char *str)
{
    while (*str)
    {
        UART0_TxChar(*str++);
    }
}

// 데이터 수신될 때까지 대기 (블로킹)
char UART0_RxChar(void)
{
    while (!(UCSR0A & (1 << RXC0)));   // 수신 완료 대기
    return UDR0;
}

// ---------------- 서보 PWM 함수 ----------------
void Servo_Init(void)
{
    DDRB |= (1 << PB5);   // PB5(OC1A) 출력 설정

    // Fast PWM, TOP = ICR1 (Mode 14)
    // COM1A1=1, COM1A0=0 -> OC1A 비반전 모드
    TCCR1A = (1 << COM1A1) | (1 << WGM11);
    // WGM13=1, WGM12=1, Prescaler=8 (CS11=1)
    TCCR1B = (1 << WGM13) | (1 << WGM12) | (1 << CS11);

    ICR1 = 39999;   // 20ms 주기 (16MHz / 8 / 50Hz - 1)

    OCR1A = PULSE_0DEG * 2;   // 초기값: 0도
}

// pulse_us 단위 pulse width를 OCR1A에 반영
void Servo_SetPulse(uint16_t pulse_us)
{
    OCR1A = pulse_us * 2;   // 1 tick = 0.5us -> *2
}

int main(void)
{
    char cmd;

    UART0_Init(9600);
    Servo_Init();

    UART0_TxString("SG90 Servo UART Test Start\r\n");
    UART0_TxString("0:0deg 1:45deg 2:90deg 3:135deg 4:180deg\r\n");

    while (1)
    {
        cmd = UART0_RxChar();

        switch (cmd)
        {
            case '0':
                Servo_SetPulse(PULSE_0DEG);
                UART0_TxString("Angle: 0 deg\r\n");
                break;

            case '1':
                Servo_SetPulse(PULSE_45DEG);
                UART0_TxString("Angle: 45 deg\r\n");
                break;

            case '2':
                Servo_SetPulse(PULSE_90DEG);
                UART0_TxString("Angle: 90 deg\r\n");
                break;

            case '3':
                Servo_SetPulse(PULSE_135DEG);
                UART0_TxString("Angle: 135 deg\r\n");
                break;

            case '4':
                Servo_SetPulse(PULSE_180DEG);
                UART0_TxString("Angle: 180 deg\r\n");
                break;
				
				

            default:
                // '0'~'4' 외 입력은 무시 (필요 시 에러 메시지 추가 가능)
                break;
        }
    }

    return 0;
}