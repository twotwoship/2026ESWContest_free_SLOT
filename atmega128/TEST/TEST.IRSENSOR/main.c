/*
 * SEN0503 IR Break Beam Sensor UART 테스트
 * ATmega128A
 *
 * 연결:
 *   - 센서 OUT(디지털 출력, NPN 오픈컬렉터) -> PD4
 *   - 센서 VCC -> 3.3~5V
 *   - 센서 GND -> ATmega128A GND와 공통 접지 필수
 *   - PD4는 내부 풀업 사용 (오픈컬렉터 출력 대응)
 *
 * 동작:
 *   - 평소(빔 연결 상태): PD4 = HIGH
 *   - 물체가 빔을 가로막으면(빔 차단): PD4 = LOW
 *   - 상태가 바뀔 때마다 UART로 메시지 출력
 *
 * UART: 9600bps, 8N1
 */
#define F_CPU 16000000UL
#include <avr/io.h>
#include <util/delay.h>

// ---------------- 센서 핀 정의 ----------------
#define SENSOR_DDR   DDRD
#define SENSOR_PORT  PORTD
#define SENSOR_PIN   PIND
#define SENSOR_BIT   PD4

// ---------------- UART 함수 (서보 코드와 동일) ----------------
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

// ---------------- 센서 함수 ----------------
void Sensor_Init(void)
{
    SENSOR_DDR  &= ~(1 << SENSOR_BIT);   // 입력
    SENSOR_PORT |=  (1 << SENSOR_BIT);   // 내부 풀업 ON
}

// 1: 빔 연결(정상), 0: 빔 차단(물체 감지)
uint8_t Sensor_Read(void)
{
    return (SENSOR_PIN & (1 << SENSOR_BIT)) ? 1 : 0;
}

int main(void)
{
    UART0_Init(9600);
    Sensor_Init();

    UART0_TxString("SEN0503 IR Break Beam UART Test Start\r\n");

    uint8_t last_state = Sensor_Read();
    UART0_TxString(last_state ? "Init: BEAM CLEAR\r\n" : "Init: BEAM BROKEN\r\n");

    while (1)
    {
        uint8_t state = Sensor_Read();

        if (state != last_state)
        {
            _delay_ms(1);              // 간단한 디바운스
            state = Sensor_Read();      // 재확인

            if (state != last_state)
            {
                last_state = state;

                if (state == 0)
                {
                    UART0_TxString("Object Detected! (BEAM BROKEN)\r\n");
                }
                else
                {
                    UART0_TxString("Beam Clear\r\n");
                }
            }
        }
    }

    return 0;
}