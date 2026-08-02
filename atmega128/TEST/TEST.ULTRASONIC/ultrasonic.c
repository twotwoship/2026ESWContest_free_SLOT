/*
 * ultrasonic.c
 *
 * TRIG(PD4) : 트리거 펄스 송신
 * ECHO(PD2, INT2) : 펄스 폭 측정 (Timer1 이용)
 */
#include "ultrasonic.h"
#include <stdio.h>

extern volatile int ultrasonic_check_time;   // main.c 의 Timer0 1ms 카운터

volatile int  ultrasonic_distance_cm = 0;    // 최종 거리값 (cm)
volatile char scm[50] = "dis: 0 cm\n";       // UART 출력 버퍼 (초기값으로 초기화해둠)

/*
 * INT2 : PD2, ECHO 핀에 논리 변화(상승/하강 엣지 둘 다)가 있을 때 진입
 *
 * *** PE4/INT4와 다른 점 ***
 * INT0~INT3 은 EICRA 레지스터로 설정 (EICRB 아님!)
 * EICRA 의 ISC21:ISC20 = 0:1 -> INT2 상승 또는 하강 엣지 모두에서 인터럽트 발생
 *
 * 측정 원리
 * 1. 상승 엣지 진입 : TCNT1 = 0 으로 리셋 (펄스 시작 시점)
 * 2. 하강 엣지 진입 : TCNT1 값으로 펄스 폭(왕복시간)을 계산
 *
 * Timer1 : 8분주 사용 (CS12:CS11:CS10 = 0:1:0)
 *   16MHz / 8 = 2MHz -> 1tick = 0.5us
 * 거리(cm) = 왕복시간(us) / 58
 *          = (TCNT1 * 0.5us) / 58
 */
// ultrasonic.c 상단에 추가
volatile uint32_t echo_int_count = 0;

ISR(INT2_vect)
{
	echo_int_count++;   // 인터럽트 진입 횟수 카운트
	
	if (ECHO_PORT & (1 << ECHO_PIN))
	{
		TCNT1 = 0;
	}
	else
	{
		uint16_t ticks = TCNT1;
		ultrasonic_distance_cm = (int)((ticks * 0.5) / 58.0);
		sprintf((char *)scm, "dis: %d cm\n", ultrasonic_distance_cm);
	}
}

void init_ultrasonic(void)
{
	// TRIG(PD4) : 출력 모드, 초기 LOW
	TRIG_DDR  |= (1 << TRIG_PIN);
	TRIG_PORT &= ~(1 << TRIG_PIN);

	// ECHO(PD2) : 입력 모드
	ECHO_DDR &= ~(1 << ECHO_PIN);

	// Timer1 : Normal mode, 8분주 (0.5us/tick)
	TCCR1A = 0x00;
	TCCR1B = 0x00;
	TCCR1B |= (1 << CS11);

	// EICRA : INT2 -> 논리 변화(양쪽 엣지)에서 인터럽트
	// ISC21 = 0, ISC20 = 1
	EICRA |= (1 << ISC20);
	EICRA &= ~(1 << ISC21);

	// INT2 인터럽트 허용
	EIMSK |= (1 << INT2);
}

void make_trigger(void)
{
	TRIG_PORT &= ~(1 << TRIG_PIN);   // LOW
	_delay_us(2);
	TRIG_PORT |= (1 << TRIG_PIN);    // HIGH
	_delay_us(15);                   // 규격 10us 이상
	TRIG_PORT &= ~(1 << TRIG_PIN);   // LOW
}

void ultrasonic_processing(void)
{
	if (ultrasonic_check_time >= 1000)   // 1초 주기
	{
		ultrasonic_check_time = 0;
		printf("%s", (char *)scm);       // 직전 측정값 출력
		make_trigger();                  // 다음 측정 트리거
	}
}