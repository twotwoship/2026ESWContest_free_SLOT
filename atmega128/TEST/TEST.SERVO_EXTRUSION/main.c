/*
 * MG996R 서보모터 각도 테스트 (버튼 입력 기반)
 * ATmega128A
 *
 * 연결:
 *   - 서보 신호선(Signal) -> PB5 (OC1A, Timer1 PWM 출력)
 *   - 서보 VCC -> 전용 5V 레일 (최소 2~3A 이상 공급 가능해야 함, MG996R stall 전류 ~2.5A)
 *                 서보 전원단에 470~1000uF 이상 저ESR 커패시터 병렬 필수
 *   - 서보 GND -> ATmega128A GND와 공통 접지 필수
 *   - 외부 버튼 -> PD0 (내부 풀업 사용, 버튼 누르면 GND로 연결되어 LOW)
 *   - UART0 TX -> PE1, RX -> PE0 (9600bps, 8N1)
 *
 * 동작 (알약 압출용):
 *   - 리셋 버튼을 누르거나 전원이 인가될 때 최초 각도: 180도
 *   - 버튼 1회 입력: 180도 -> 50도로 즉시 이동 (130도 이동)
 *   - 이후 별도 입력 없이 자동으로 0.5초 간격 1도씩 감소 (49, 48, ... 0도)
 *     -> 알약을 지긋이 눌러 압출
 *   - 0도 도달 후에는 버튼을 눌러도 더 이상 이동하지 않음
 *   - 각도가 바뀔 때마다 UART로 현재 각도 출력
 *
 * PWM: Timer1, Fast PWM (Mode 14, TOP=ICR1), 50Hz (20ms 주기)
 *      F_CPU = 16MHz, Prescaler = 8 -> 1 tick = 0.5us
 *      ICR1 = 39999  (20ms 주기)
 *      OCR1A = pulse_width(us) * 2
 *
 * 주의: MG996R은 개체별 펄스폭 편차가 있을 수 있음.
 *       PULSE_MIN_US / PULSE_MAX_US는 실측 후 조정 권장 (0도/180도 근처 동작 확인).
 */
#define F_CPU 16000000UL
#include <avr/io.h>
#include <util/delay.h>
#include <stdlib.h>

// 버튼 핀 정의
#define BUTTON_PIN   PD0
#define BUTTON_PORT  PORTD
#define BUTTON_DDR   DDRD
#define BUTTON_PINREG PIND

// 각도 범위 정의
#define ANGLE_INIT      180u
#define ANGLE_AFTER_1ST 50u
#define ANGLE_STEP      5u
#define ANGLE_MAX       0u
#define ANGLE_STEP_DELAY_MS 500u   // 압출 단계 간격 (0.5초)

// 서보 펄스폭 범위 (us) - MG996R 실측 후 조정 권장
#define PULSE_MIN_US    500u    // 0도
#define PULSE_MAX_US    2500u   // 180도

// UART 설정
#define BAUD 9600UL
#define UBRR_VALUE ((F_CPU / (16UL * BAUD)) - 1)

void Servo_Init(void)
{
    DDRB |= (1 << PB5);   // PB5(OC1A) 출력 설정
    // Fast PWM, TOP = ICR1 (Mode 14)
    TCCR1A = (1 << COM1A1) | (1 << WGM11);
    TCCR1B = (1 << WGM13) | (1 << WGM12) | (1 << CS11);
    ICR1 = 39999;   // 20ms 주기
}

void Servo_SetPulse(uint16_t pulse_us)
{
    OCR1A = pulse_us * 2;   // 1 tick = 0.5us -> *2
}

// 각도(0~180) -> 펄스폭(us) 변환
// PULSE_MIN_US(0도) ~ PULSE_MAX_US(180도) 선형 매핑
uint16_t Angle_ToPulse(uint8_t angle_deg)
{
    uint32_t range = (uint32_t)(PULSE_MAX_US - PULSE_MIN_US);
    return (uint16_t)(PULSE_MIN_US + (range * angle_deg) / 180u);
}

void Button_Init(void)
{
    BUTTON_DDR &= ~(1 << BUTTON_PIN);   // 입력 설정
    BUTTON_PORT |= (1 << BUTTON_PIN);   // 내부 풀업 활성화
}

// 버튼이 눌렸다가(LOW) 떼어질 때(HIGH)까지 대기하며 디바운스 처리
// 반환값: 1 = 눌림 확정, 0 = 눌림 없음
uint8_t Button_CheckPress(void)
{
    if (!(BUTTON_PINREG & (1 << BUTTON_PIN)))   // LOW = 눌림
    {
        _delay_ms(20);   // 디바운스
        if (!(BUTTON_PINREG & (1 << BUTTON_PIN)))
        {
            while (!(BUTTON_PINREG & (1 << BUTTON_PIN)))
            {
                // 버튼 떼어질 때까지 대기 (하나의 누름 = 하나의 이벤트)
            }
            _delay_ms(20);   // 릴리즈 디바운스
            return 1;
        }
    }
    return 0;
}

// UART0 초기화 (9600bps, 8N1)
void UART_Init(void)
{
    UBRR0H = (uint8_t)(UBRR_VALUE >> 8);
    UBRR0L = (uint8_t)UBRR_VALUE;
    UCSR0B = (1 << TXEN0);                     // 송신 활성화
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);    // 8비트, 1스톱, 패리티 없음
}

void UART_TxChar(char data)
{
    while (!(UCSR0A & (1 << UDRE0)))   // 송신 버퍼 비워질 때까지 대기
        ;
    UDR0 = data;
}

void UART_TxString(const char *str)
{
    while (*str)
    {
        UART_TxChar(*str);
        str++;
    }
}

// 현재 각도를 "Current angle: XXX deg\r\n" 형태로 출력
void UART_PrintAngle(uint8_t angle)
{
    char buf[6];
    UART_TxString("Current angle: ");
    itoa(angle, buf, 10);
    UART_TxString(buf);
    UART_TxString(" deg\r\n");
}

int main(void)
{
    uint8_t current_angle = ANGLE_INIT;

    Servo_Init();
    Button_Init();
    UART_Init();

    Servo_SetPulse(Angle_ToPulse(current_angle));   // 초기 180도로 세팅
    UART_PrintAngle(current_angle);                 // 초기 각도 출력

    while (1)
    {
        if (Button_CheckPress())
        {
            if (current_angle == ANGLE_INIT)
            {
                current_angle = ANGLE_AFTER_1ST;    // 180 -> 50 (130도 즉시 이동)
                Servo_SetPulse(Angle_ToPulse(current_angle));
                UART_PrintAngle(current_angle);

                // 이후 0.5초 간격으로 1도씩 감소시키며 0도까지 압출
                while (current_angle > ANGLE_MAX)
                {
                    _delay_ms(ANGLE_STEP_DELAY_MS);
                    current_angle -= ANGLE_STEP;
                    Servo_SetPulse(Angle_ToPulse(current_angle));
                    UART_PrintAngle(current_angle);
                }
            }
            // current_angle == ANGLE_MAX(0)이면 더 이상 이동하지 않음
        }
    }

    return 0;
}