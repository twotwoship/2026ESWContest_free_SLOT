/*
 * calib_main.c
 * SLOT-GUARD 스텝모터 캘리브레이션 전용 펌웨어
 *
 * 본 SLOT-GUARD 본 펌웨어(main.c)와는 별개의 프로젝트로 빌드해서 사용할 것.
 * 캘리브레이션이 끝나면 이 펌웨어는 더 이상 필요 없고, motor.c의
 * X_SLOT_STEPS[] / Y_SLOT_STEPS[] 배열에 실측값만 옮겨 적으면 됨.
 *
 * 필요 파일: step_motor.c/.h, timer.c/.h, uart_driver.c, calib_main.c
 * (uart_protocol.c, fsm.c, eeprom_state.c, motor.c, tasks.c, main.c는 포함하지 말 것 -
 *  main() 중복 및 불필요한 의존성 때문)
 *
 * ComPort Master에서 9600bps 8N1로 접속 후 아래 명령을 라인 단위(LF 종료)로 전송:
 *   X+       X축을 기본 스텝(20)만큼 정방향 이동
 *   X-       X축을 기본 스텝만큼 역방향 이동
 *   X+50     X축을 50스텝 정방향 이동 (숫자로 스텝수 직접 지정)
 *   X-50     X축을 50스텝 역방향 이동
 *   Y+ / Y- / Y+N / Y-N   Y축도 동일
 *   P        현재 누적 절대 스텝 위치 출력
 *   Z        현재 위치를 원점(0,0)으로 재설정 (실제로 모터를 움직이진 않음 - 표시값만 초기화)
 *
 * 사용 순서 예시(X축 행 1 실측):
 *   1) 기구를 손으로 행 0 위치에 맞춰놓고 'Z' 전송 (원점 표시 초기화)
 *   2) 'X+50' 등을 반복 전송하며 실제로 행 1 슬롯에 정확히 도착할 때까지 미세 조정
 *      (오버슈트했으면 'X-10'처럼 음수 방향으로 되돌릴 수 있음)
 *   3) 도착하면 'P' 전송 -> 출력된 "POS X=..." 값을 motor.c의 X_SLOT_STEPS[1]에 기록
 *   4) Y축도 동일한 방식으로 열 1~4 각각 실측
 */
#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "step_motor.h"
#include "timer.h"

// ---- uart_driver.c 에 정의된 저수준 함수/링버퍼를 그대로 재사용 ----
extern void    UART0_Init(unsigned long baud);
extern void    UART0_TxChar(char data);
extern void    UART0_TxString(const char *str);
extern uint8_t RingBuf_Pop(uint8_t *out);

#define STEP_INCREMENT   20   // 명령에 숫자를 안 붙였을 때 기본 이동 스텝수
#define LINE_MAX_LEN     31

static int32_t s_pos_x = 0;   // 화면 표시/기록용 누적 절대 위치 (실제 스텝모터 내부 카운터와 별개)
static int32_t s_pos_y = 0;

static void print_pos(void)
{
    char buf[12];

    UART0_TxString("POS X=");
    ltoa(s_pos_x, buf, 10);
    UART0_TxString(buf);
    UART0_TxString(" Y=");
    ltoa(s_pos_y, buf, 10);
    UART0_TxString(buf);
    UART0_TxString("\r\n");
}

// 지정한 축을 delta(부호 있음) 스텝만큼 이동시키고, 완료될 때까지 대기(busy-wait)한다.
// 캘리브레이션 전용 도구라 non-blocking일 필요가 없어 단순하게 구현.
static void move_axis(StepperAxis_t axis, int32_t delta)
{
    if (delta == 0) return;

    uint8_t  dir   = (delta >= 0) ? 0 : 1;
    uint16_t steps = (uint16_t)((delta >= 0) ? delta : -delta);

    Stepper_MoveSteps(axis, dir, steps, Tick_GetMs());

    while (Stepper_IsMoving(axis))
    {
        Stepper_Update(Tick_GetMs());
    }

    if (axis == AXIS_X) s_pos_x += delta;
    else                s_pos_y += delta;
}

static void handle_line(char *line)
{
    uint8_t len = (uint8_t)strlen(line);
    if (len == 0) return;

    char cmd  = line[0];
    char sign = (len >= 2) ? line[1] : '\0';

    int32_t amount = STEP_INCREMENT;
    if (len > 2)
    {
        long parsed = atol(&line[2]);
        if (parsed > 0) amount = parsed;
    }

    if (cmd == 'X' || cmd == 'x')
    {
        if (sign != '+' && sign != '-')
        {
            UART0_TxString("? 형식: X+ / X- / X+50 / X-50\r\n");
            return;
        }
        move_axis(AXIS_X, (sign == '-') ? -amount : amount);
        print_pos();
    }
    else if (cmd == 'Y' || cmd == 'y')
    {
        if (sign != '+' && sign != '-')
        {
            UART0_TxString("? 형식: Y+ / Y- / Y+50 / Y-50\r\n");
            return;
        }
        move_axis(AXIS_Y, (sign == '-') ? -amount : amount);
        print_pos();
    }
    else if (cmd == 'P' || cmd == 'p')
    {
        print_pos();
    }
    else if (cmd == 'Z' || cmd == 'z')
    {
        s_pos_x = 0;
        s_pos_y = 0;
        UART0_TxString("ZEROED (표시 위치를 0,0으로 재설정 - 실제 모터는 움직이지 않음)\r\n");
    }
    else
    {
        UART0_TxString("? 명령: X+/X-/Y+/Y- (숫자로 스텝수 지정 가능, 예 X+50), P=현재위치, Z=원점재설정\r\n");
    }
}

int main(void)
{
    UART0_Init(9600);
    Timer2_Init();
    Stepper_Init();
    sei();

    UART0_TxString("=== SLOT-GUARD Stepper Calibration Mode ===\r\n");
    UART0_TxString("Commands: X+ X- Y+ Y- (default 20 steps, e.g. X+50), P=position, Z=reset origin\r\n");
    print_pos();

    static char line[LINE_MAX_LEN + 1];
    uint8_t len = 0;

    while (1)
    {
        uint8_t byte;
        while (RingBuf_Pop(&byte))
        {
            if (byte == '\n')
            {
                line[len] = '\0';
                if (len > 0 && line[len - 1] == '\r')
                {
                    line[len - 1] = '\0';
                }
                handle_line(line);
                len = 0;
            }
            else if (byte != '\r' && len < LINE_MAX_LEN)
            {
                line[len++] = (char)byte;
            }
        }
    }
}
