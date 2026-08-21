/*
 * 28BYJ-48 + ULN2003 스테퍼 모터 - 스텝 수 실측(캘리브레이션) 테스트
 * ATmega128A, F_CPU = 16MHz, UART0 9600bps
 *
 * 배선:
 *   PA0 -> ULN2003 IN1
 *   PA1 -> ULN2003 IN2
 *   PA2 -> ULN2003 IN3
 *   PA3 -> ULN2003 IN4
 *   ULN2003 VDD -> 외부 5V (CP2102 5V)
 *   ULN2003 GND -> 공통 GND
 *
 * UART 명령 (터미널에서 문자 입력, Enter 불필요):
 *   'g' : 정방향 연속 회전 시작
 *   'r' : 역방향 연속 회전 시작
 *   ' '(스페이스) : 즉시 정지 + 누적 스텝 수 / 경과 시간(ms) 출력
 */
#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include "step_motor.h"

/* ================= 공통 tick (Timer2, 1ms) =================
 * ATmega128 Timer2는 (ATmega8/328p와 달리) 비동기(async) 지원 타이머가 아니라
 * Timer1과 같은 5단계 표준 분주 테이블을 쓴다. CS21:CS20=11 -> clk/64.
 */
static volatile uint16_t g_tick_ms = 0;

static void Timer2_Init(void)
{
	TCCR2 = (1 << WGM21) | (1 << CS21) | (1 << CS20);   // CTC 모드, prescaler 64
	OCR2  = 249;                                          // 250카운트 -> 1ms
	TIMSK |= (1 << OCIE2);
}

ISR(TIMER2_COMP_vect)
{
	g_tick_ms++;
}

static uint16_t get_tick_safe(void)
{
	uint16_t t;
	cli();
	t = g_tick_ms;
	sei();
	return t;
}

/* ================= UART0 ================= */
#define UART0_BAUD 9600

static volatile char    s_key_char  = 0;
static volatile uint8_t s_key_ready = 0;

static void UART0_Init(void)
{
	uint16_t ubrr = (F_CPU / 16 / UART0_BAUD) - 1;

	UBRR0H = (uint8_t)(ubrr >> 8);
	UBRR0L = (uint8_t)ubrr;

	UCSR0B = (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);
	UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

static void UART0_TxChar(char c)
{
	while (!(UCSR0A & (1 << UDRE0)));
	UDR0 = c;
}

static void UART0_TxString(const char *str)
{
	while (*str)
	{
		UART0_TxChar(*str++);
	}
}

static void UART0_TxUint16(uint16_t num)
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

// 문자 1개를 받는 즉시 명령으로 확정 (Enter 불필요)
ISR(USART0_RX_vect)
{
	char c = UDR0;

	if (s_key_ready)
	{
		return; // 이전 키가 아직 처리되지 않음
	}

	s_key_char  = c;
	s_key_ready = 1;
}

static uint8_t UART0_KeyReady(void)
{
	return s_key_ready;
}

static char UART0_ReadKey(void)
{
	char c = s_key_char;

	s_key_ready = 0;

	return c;
}

/* ================= main ================= */
int main(void)
{
	UART0_Init();
	Timer2_Init();
	Stepper_Init();
	sei();

	UART0_TxString("Step Calibration Mode\r\n");
	UART0_TxString("g=forward run, r=reverse run, SPACE=stop & report\r\n");

	uint16_t run_start_tick = 0;

	while (1)
	{
		uint16_t now = get_tick_safe();

		Stepper_Update(now);

		if (UART0_KeyReady())
		{
			char key = UART0_ReadKey();

			if (key == 'g' || key == 'G')
			{
				Stepper_StartContinuous(0, now);
				run_start_tick = now;
				UART0_TxString("RUN FORWARD...\r\n");
			}
			else if (key == 'r' || key == 'R')
			{
				Stepper_StartContinuous(1, now);
				run_start_tick = now;
				UART0_TxString("RUN REVERSE...\r\n");
			}
			else if (key == ' ')
			{
				if (Stepper_IsMoving())
				{
					Stepper_Stop();

					uint16_t steps   = Stepper_GetStepCount();
					uint16_t elapsed = (uint16_t)(now - run_start_tick);

					UART0_TxString("STOP: steps=");
					UART0_TxUint16(steps);
					UART0_TxString(" time=");
					UART0_TxUint16(elapsed);
					UART0_TxString(" ms\r\n");
				}
				else
				{
					UART0_TxString("NOT RUNNING\r\n");
				}
			}
		}
	}

	return 0;
}
