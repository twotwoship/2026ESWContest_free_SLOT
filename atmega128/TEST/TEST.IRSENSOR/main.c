#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>

// ---------------- 센서 핀 정의 (IR1 = PD2/INT2, IR2 = PE4/INT4) ----------------
// PD3(INT3)는 하드웨어 결함(핀 고착 LOW)으로 사용 불가 -> PE4(INT4)로 이전
#define SENSOR1_DDR   DDRD
#define SENSOR1_PORT  PORTD
#define SENSOR1_PIN   PIND
#define SENSOR1_BIT   PD2

#define SENSOR2_DDR   DDRE
#define SENSOR2_PORT  PORTE
#define SENSOR2_PIN   PINE
#define SENSOR2_BIT   PE4

#define DEBOUNCE_MS  5      // 디바운스 시간 (ms)

// ---------------- 전역 변수 ----------------
volatile uint8_t  g_sensor_event = 0;   // ISR(IR1 또는 IR2)에서 세우는 이벤트 플래그
volatile uint16_t g_tick_ms      = 0;   // 1ms마다 증가하는 tick 카운터
volatile uint16_t g_last_event_tick = 0;
volatile uint16_t g_pill_count   = 0;   // 감지 카운트 (IR1, IR2 중 하나라도 감지되면 증가)

uint8_t last_state1;   // IR1 최근 상태
uint8_t last_state2;   // IR2 최근 상태

// ---------------- UART 함수 ----------------
void UART0_Init(unsigned long baud)
{
	unsigned int ubrr = (F_CPU / 16 / baud) - 1;
	UBRR0H = (unsigned char)(ubrr >> 8);
	UBRR0L = (unsigned char)ubrr;
	UCSR0B = (1 << TXEN0) | (1 << RXEN0);
	UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

void UART0_TxChar(char data)
{
	while (!(UCSR0A & (1 << UDRE0)));
	UDR0 = data;
}

void UART0_TxString(const char *str)
{
	while (*str)
	{
		UART0_TxChar(*str++);
	}
}

// 숫자(uint16_t)를 문자열로 변환해서 UART로 출력
void UART0_TxUint16(uint16_t num)
{
	char buf[6];
	uint8_t i = 0;

	if (num == 0)
	{
		UART0_TxChar('0');
		return;
	}

	while (num > 0)
	{
		buf[i++] = '0' + (num % 10);
		num /= 10;
	}

	while (i > 0)
	{
		UART0_TxChar(buf[--i]);
	}
}

// ---------------- Timer2: 1ms tick 생성 (CTC 모드) ----------------
void Timer2_Init(void)
{
	TCCR2 = (1 << WGM21) | (1 << CS22);   // CTC 모드, prescaler 64
	OCR2  = 249;                           // 250 카운트 -> 1ms
	TIMSK |= (1 << OCIE2);
}

ISR(TIMER2_COMP_vect)
{
	g_tick_ms++;
}

// ---------------- 센서 초기화 & 외부 인터럽트(INT2, INT4) 설정 ----------------
void Sensor_Init(void)
{
	// IR1 (PD2 / INT2)
	SENSOR1_DDR  &= ~(1 << SENSOR1_BIT);
	SENSOR1_PORT |=  (1 << SENSOR1_BIT);

	// IR2 (PE4 / INT4)
	SENSOR2_DDR  &= ~(1 << SENSOR2_BIT);
	SENSOR2_PORT |=  (1 << SENSOR2_BIT);

	// INT2: any logical change (ISC21:ISC20 = 01) - EICRA
	EICRA &= ~((1 << ISC21) | (1 << ISC20));
	EICRA |=  (1 << ISC20);

	// INT4: any logical change (ISC41:ISC40 = 01) - EICRB
	EICRB &= ~((1 << ISC41) | (1 << ISC40));
	EICRB |=  (1 << ISC40);

	EIMSK |= (1 << INT2) | (1 << INT4);
}

// 1: 빔 연결(정상), 0: 빔 차단(물체 감지)
uint8_t Sensor1_Read(void)
{
	return (SENSOR1_PIN & (1 << SENSOR1_BIT)) ? 1 : 0;
}

uint8_t Sensor2_Read(void)
{
	return (SENSOR2_PIN & (1 << SENSOR2_BIT)) ? 1 : 0;
}

// ISR은 가볍게 - 플래그만 세움
ISR(INT2_vect)
{
	g_sensor_event = 1;
	g_last_event_tick = g_tick_ms;
}

ISR(INT4_vect)
{
	g_sensor_event = 1;
	g_last_event_tick = g_tick_ms;
}

uint16_t get_tick_safe(void)
{
	uint16_t t;
	cli();
	t = g_tick_ms;
	sei();
	return t;
}

int main(void)
{
	// MCU가 왜 (재)부팅됐는지 확인용 - 다른 초기화보다 먼저 읽고 지워야 함
	uint8_t reset_cause = MCUCSR;
	MCUCSR = 0;

	UART0_Init(9600);
	Timer2_Init();
	Sensor_Init();
	sei();

	UART0_TxString("SEN0503 IR Break Beam INT2(PD2)/INT4(PE4) Dual Sensor Test Start\r\n");

	UART0_TxString("Reset Cause: ");
	if (reset_cause & (1 << PORF))  UART0_TxString("POWER-ON ");
	if (reset_cause & (1 << EXTRF)) UART0_TxString("EXTERNAL ");
	if (reset_cause & (1 << BORF))  UART0_TxString("BROWN-OUT ");
	if (reset_cause & (1 << WDRF))  UART0_TxString("WATCHDOG ");
	if (reset_cause & (1 << JTRF))  UART0_TxString("JTAG ");
	UART0_TxString("\r\n");

	last_state1 = Sensor1_Read();
	last_state2 = Sensor2_Read();
	UART0_TxString(last_state1 ? "Init IR1: BEAM CLEAR\r\n" : "Init IR1: BEAM BROKEN\r\n");
	UART0_TxString(last_state2 ? "Init IR2: BEAM CLEAR\r\n" : "Init IR2: BEAM BROKEN\r\n");

	while (1)
	{
		if (g_sensor_event)
		{
			//if ((uint16_t)(get_tick_safe() - g_last_event_tick) >= DEBOUNCE_MS)
			if (1)
			{
				cli();
				g_sensor_event = 0;
				sei();

				uint8_t state1 = Sensor1_Read();
				uint8_t state2 = Sensor2_Read();

				// 각 센서는 반드시 "자기 자신의 이전 상태"와만 비교한다.
				// (한쪽을 합쳐진 공통 latch로 판정하면, 한쪽 센서가 정렬 불량 등으로
				//  BROKEN 상태에 고착될 때 다른 센서의 새 감지까지 영원히 막혀버린다.)
				uint8_t edge1_detected = (state1 == 0) && (last_state1 == 1); // IR1: CLEAR->BROKEN
				uint8_t edge2_detected = (state2 == 0) && (last_state2 == 1); // IR2: CLEAR->BROKEN
				uint8_t edge1_cleared  = (state1 == 1) && (last_state1 == 0); // IR1: BROKEN->CLEAR
				uint8_t edge2_cleared  = (state2 == 1) && (last_state2 == 0); // IR2: BROKEN->CLEAR

				last_state1 = state1;
				last_state2 = state2;

				if (edge1_detected || edge2_detected)
				{
					g_pill_count++;   // 둘 중 하나만 감지되어도 카운트 증가 (동시 감지 시 1회만 증가)

					UART0_TxString("Object Detected! (BEAM BROKEN) [");
					if (edge1_detected && edge2_detected)
					{
						UART0_TxString("IR1+IR2");
					}
					else if (edge1_detected)
					{
						UART0_TxString("IR1");
					}
					else
					{
						UART0_TxString("IR2");
					}
					UART0_TxString("] Count=");
					UART0_TxUint16(g_pill_count);
					UART0_TxString("\r\n");
				}

				if (edge1_cleared || edge2_cleared)
				{
					UART0_TxString("Beam Clear [");
					if (edge1_cleared && edge2_cleared)
					{
						UART0_TxString("IR1+IR2");
					}
					else if (edge1_cleared)
					{
						UART0_TxString("IR1");
					}
					else
					{
						UART0_TxString("IR2");
					}
					UART0_TxString("]\r\n");
				}
			}
		}

		// 여기서 다른 작업(스텝모터 제어 등)을 non-blocking으로 처리 가능
	}

	return 0;
}
#if 0
/*
 * SEN0503 IR Break Beam Sensor - 외부 인터럽트(INT2) 방식
 * ATmega128A
 *
 * 연결:
 *   - 센서 OUT(디지털 출력, NPN 오픈컬렉터) -> PD2 (INT2)
 *     * PD4는 ATmega128A에서 외부 인터럽트 핀이 아니므로 PD2로 이동
 *   - 센서 VCC -> 3.3~5V
 *   - 센서 GND -> ATmega128A GND와 공통 접지 필수
 *   - PD2는 내부 풀업 사용 (오픈컬렉터 출력 대응)
 *
 * 동작:
 *   - INT2를 "any logical change"(양쪽 edge 모두) 모드로 설정
 *   - 신호 변화 시 ISR에서는 플래그만 세우고, 실제 디바운스/판정은
 *     메인 루프에서 Timer2 1ms tick 기준으로 처리 (non-blocking)
 *
 * UART: 9600bps, 8N1
 */
#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>

// ---------------- 센서 핀 정의 (PD2 = INT2) ----------------
#define SENSOR_DDR   DDRD
#define SENSOR_PORT  PORTD
#define SENSOR_PIN   PIND
#define SENSOR_BIT   PD2

#define DEBOUNCE_MS  5      // 디바운스 시간 (ms)

// ---------------- 전역 변수 ----------------
volatile uint8_t  g_sensor_event = 0;   // ISR에서 세우는 이벤트 플래그
volatile uint16_t g_tick_ms      = 0;   // 1ms마다 증가하는 tick 카운터
volatile uint16_t g_last_event_tick = 0;

uint8_t last_state;

// ---------------- UART 함수 ----------------
void UART0_Init(unsigned long baud)
{
    unsigned int ubrr = (F_CPU / 16 / baud) - 1;
    UBRR0H = (unsigned char)(ubrr >> 8);
    UBRR0L = (unsigned char)ubrr;
    UCSR0B = (1 << TXEN0) | (1 << RXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

void UART0_TxChar(char data)
{
    while (!(UCSR0A & (1 << UDRE0)));
    UDR0 = data;
}

void UART0_TxString(const char *str)
{
    while (*str)
    {
        UART0_TxChar(*str++);
    }
}

// ---------------- Timer2: 1ms tick 생성 (CTC 모드) ----------------
// F_CPU=16MHz, prescaler=64 -> 250,000 tick/s -> 250카운트 = 1ms
void Timer2_Init(void)
{
    TCCR2 = (1 << WGM21) | (1 << CS22);   // CTC 모드, prescaler 64
    OCR2  = 249;                           // 250 카운트 -> 1ms
    TIMSK |= (1 << OCIE2);                 // Output Compare Interrupt 허용
}

ISR(TIMER2_COMP_vect)
{
    g_tick_ms++;
}

// ---------------- 센서 초기화 & 외부 인터럽트(INT2) 설정 ----------------
void Sensor_Init(void)
{
    SENSOR_DDR  &= ~(1 << SENSOR_BIT);   // 입력
    SENSOR_PORT |=  (1 << SENSOR_BIT);   // 내부 풀업 ON

    // INT2: any logical change (ISC21:ISC20 = 01)
    EICRA &= ~((1 << ISC21) | (1 << ISC20));
    EICRA |=  (1 << ISC20);

    EIMSK |= (1 << INT2);   // INT2 인터럽트 허용
}

// 1: 빔 연결(정상), 0: 빔 차단(물체 감지)
uint8_t Sensor_Read(void)
{
    return (SENSOR_PIN & (1 << SENSOR_BIT)) ? 1 : 0;
}

// ISR은 가볍게 - 플래그만 세움
ISR(INT2_vect)
{
    g_sensor_event = 1;
    g_last_event_tick = g_tick_ms;
}

// tick 값 안전하게 읽기 (16bit라 인터럽트 중 값 깨질 수 있음)
uint16_t get_tick_safe(void)
{
    uint16_t t;
    cli();
    t = g_tick_ms;
    sei();
    return t;
}

int main(void)
{
    UART0_Init(9600);
    Timer2_Init();
    Sensor_Init();
    sei();   // 전역 인터럽트 허용

    UART0_TxString("SEN0503 IR Break Beam INT2 Test Start\r\n");

    last_state = Sensor_Read();
    UART0_TxString(last_state ? "Init: BEAM CLEAR\r\n" : "Init: BEAM BROKEN\r\n");

    while (1)
    {
        if (g_sensor_event)
        {
            // 디바운스: 이벤트 발생 후 DEBOUNCE_MS 지날 때까지 대기 후 재확인
            //if ((uint16_t)(get_tick_safe() - g_last_event_tick) >= DEBOUNCE_MS)
			if(1)
            {
                cli();
                g_sensor_event = 0;   // 플래그 클리어
                sei();

                uint8_t state = Sensor_Read();
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

        // 여기서 다른 작업(스텝모터 제어 등)을 non-blocking으로 처리 가능
    }

    return 0;
}
#endif

#if 0
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

volatile int cnt = 0;

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
            _delay_us(100);              // 간단한 디바운스
            state = Sensor_Read();      // 재확인

            if (state != last_state)
            {
                last_state = state;

                if (state == 0)
                {
					cnt++;
                    UART0_TxString("Object Detected! (BEAM BROKEN) \r\n");
					
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
#endif