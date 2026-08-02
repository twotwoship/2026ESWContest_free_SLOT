/*
 * ultrasonic.h
 *
 * ATmega128A 초음파 센서(HC-SR04) 거리 측정
 * TRIG : PD4 (출력)
 * ECHO : PD2 (INT2, 입력)
 */
#ifndef ULTRASONIC_H_
#define ULTRASONIC_H_

#include <avr/io.h>
#include <util/delay.h>
#include <avr/interrupt.h>

#ifndef F_CPU
#define F_CPU 16000000UL
#endif

// TRIG : PD4 (출력, 트리거 펄스 송신)
#define TRIG_DDR   DDRD
#define TRIG_PORT  PORTD
#define TRIG_PIN   4

// ECHO : PD2 (INT2, 입력, 에코 펄스 수신)
#define ECHO_DDR   DDRD
#define ECHO_PORT  PIND
#define ECHO_PIN   2

void init_ultrasonic(void);
void make_trigger(void);
void ultrasonic_processing(void);

#endif /* ULTRASONIC_H_ */