/**
 * @file    sensors.c
 * @brief   IR x2 + 홈 센서 x2 통합 드라이버 구현
 *
 * 핀맵 (config.h §1.7 / §1.8)
 *   홈 X : PD0 / INT0 / 활성 HIGH / 외부 풀업 4.7k -> 내부 풀업 OFF
 *   홈 Y : PD1 / INT1 / 활성 HIGH / 외부 풀업 4.7k -> 내부 풀업 OFF
 *   IR1  : PD2 / INT2 / 활성 LOW  / NPN 오픈 컬렉터 -> 내부 풀업 ON
 *   IR2  : PE4 / INT4 / 활성 LOW  / NPN 오픈 컬렉터 -> 내부 풀업 ON
 *
 * 트리거 조건
 *   홈 : rising edge  (비활성 LOW -> 활성 HIGH)
 *   IR : falling edge (비활성 HIGH -> 활성 LOW)
 *
 * ATmega128 은 EICRA 가 INT0~INT3 에 각 2비트씩(ISC31..ISC00) 배정되어 있어
 * 네 가지 트리거를 모두 지원한다. INT4~INT7 은 EICRB.
 *
 * 비트 조작은 avr-libc 의 _BV() 매크로 대신 (1 << n) 을 직접 쓴다.
 * _BV(n) 은 (1 << (n)) 과 완전히 동일한 매크로이며 생성 코드도 같다.
 * 1 은 int 리터럴이라 8비트 레지스터에 대입/AND 할 때 폭이 안 맞을 수 있어
 * 관련 대입/마스크 연산에는 전부 (uint8_t) 캐스팅을 붙인다.
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/atomic.h>

#include "sensors.h"
#include "systick.h"

/* ==========================================================================
 * 내부 상태
 * --------------------------------------------------------------------------
 * [수정] types.h 의 SensorCtx 를 쓰지 않는다. home_x_latched/home_y_latched
 * 개별 필드 구조라 AxisId 기반 API(Sensors_HomeLevel(AxisId) 등)와 맞지
 * 않고, home_poll_tick_ms(레벨 폴링 기준 시각) 필드도 없다. 내부 context
 * struct 는 .c 소유 원칙에 따라 여기서 배열 기반으로 다시 정의한다.
 * ========================================================================== */

typedef struct {
    volatile bool ir_latched;
    volatile bool home_latched[AXIS_COUNT];
    uint32_t      home_poll_tick_ms;
} SensorCtx;

static SensorCtx s_ctx;

/* ==========================================================================
 * 초기화
 * ========================================================================== */

void Sensors_Init(void)
{
    uint8_t i;

    /* --- 내부 상태 초기화 ------------------------------------------------ */
    s_ctx.ir_latched        = false;
    s_ctx.home_poll_tick_ms = 0UL;
    for (i = 0U; i < (uint8_t)AXIS_COUNT; i++) {
        s_ctx.home_latched[i] = false;
    }

    /* --- 1. 방향 레지스터: 4핀 모두 입력 -------------------------------- */
    /* 대입(=)이 아니라 비트 클리어(&=~)로 처리해야 같은 포트의 다른 핀을
     * 건드리지 않는다. PORTD/PORTE 는 다른 모듈도 쓴다. */
    HOME_X_DDR &= (uint8_t)~(1U << HOME_X_BIT);
    HOME_Y_DDR &= (uint8_t)~(1U << HOME_Y_BIT);
    IR1_DDR    &= (uint8_t)~(1U << IR1_BIT);
    IR2_DDR    &= (uint8_t)~(1U << IR2_BIT);

    /* --- 2. 풀업: 홈은 OFF, IR 은 ON ------------------------------------ */
    /* 이 모듈이 존재하는 핵심 이유. 같은 PORTD 안에서 정책이 반대다. */
#if (HOME_INTERNAL_PULLUP != 0U)
    HOME_X_PORT |= (uint8_t)(1U << HOME_X_BIT);
    HOME_Y_PORT |= (uint8_t)(1U << HOME_Y_BIT);
#else
    HOME_X_PORT &= (uint8_t)~(1U << HOME_X_BIT);
    HOME_Y_PORT &= (uint8_t)~(1U << HOME_Y_BIT);
#endif

#if (IR_INTERNAL_PULLUP != 0U)
    IR1_PORT |= (uint8_t)(1U << IR1_BIT);
    IR2_PORT |= (uint8_t)(1U << IR2_BIT);
#else
    IR1_PORT &= (uint8_t)~(1U << IR1_BIT);
    IR2_PORT &= (uint8_t)~(1U << IR2_BIT);
#endif

    /* --- 3. 트리거 조건 --------------------------------------------------
     * EICRA: [ISC31 ISC30 ISC21 ISC20 ISC11 ISC10 ISC01 ISC00]
     *        11 = rising edge, 10 = falling edge
     *   INT0(홈X) = 11, INT1(홈Y) = 11, INT2(IR1) = 10, INT3 = 미사용(보존)
     * EICRB: INT4(IR2) = 10
     * 통째 대입하지 않고 해당 비트만 갱신한다.
     */
    EICRA = (uint8_t)((EICRA & 0xC0U)                       /* INT3 비트 보존 */
                    | (uint8_t)(0x02U << ISC20)              /* INT2 falling */
                    | (uint8_t)(0x03U << ISC10)              /* INT1 rising  */
                    | (uint8_t)(0x03U << ISC00));            /* INT0 rising  */

    EICRB = (uint8_t)((EICRB & (uint8_t)~(0x03U << ISC40))   /* INT5~7 보존 */
                    | (uint8_t)(0x02U << ISC40));            /* INT4 falling */

    /* --- 4. 잔류 플래그 제거 ---------------------------------------------
     * EIFR 은 1을 써야 지워진다. 트리거 조건을 바꾸는 과정에서
     * 가짜 엣지가 검출될 수 있으므로 EIMSK 를 열기 전에 반드시 지운다.
     */
    EIFR = (uint8_t)((1U << INTF0) | (1U << INTF1) | (1U << INTF2) | (1U << INTF4));

    /* --- 5. 인터럽트 마스크 ----------------------------------------------
     * 홈은 항상 열어둔다. IR 은 Sensors_IrEnable() 로만 연다.
     */
    EIMSK &= (uint8_t)~((1U << INT2) | (1U << INT4));
    EIMSK |= (uint8_t)((1U << INT0) | (1U << INT1));

    /* --- 6. 부팅 시점 레벨 반영 ------------------------------------------
     * 이미 원점에 정지한 채 부팅하면 rising edge 가 발생하지 않는다.
     * 초기 레벨을 여기서 한 번 래치에 반영한다.
     */
    for (i = 0U; i < (uint8_t)AXIS_COUNT; i++) {
        if (Sensors_HomeLevel((AxisId)i)) {
            s_ctx.home_latched[i] = true;
        }
    }
}

/* ==========================================================================
 * 슈퍼루프 tick
 * ========================================================================== */

void Sensors_Task(void)
{
    /* IR 은 ISR 이 s_ctx.ir_latched 를 직접 세트한다 — Task 개입 없음.
     * (설계서 §6 순수 falling-edge. 재확인 디바운스는 빔을 3~8ms 만에
     *  통과하는 빠른 낙하를 스파이크로 오인해 놓친다. 전기 노이즈는 §7 범위 밖.) */

    /* 홈 센서 레벨 폴링 — 엣지 래치의 보완 (엣지 놓침 / 이미 원점 위). */
    if (Systick_Elapsed(s_ctx.home_poll_tick_ms, HOME_LEVEL_POLL_MS)) {
        uint8_t i;
        s_ctx.home_poll_tick_ms = Systick_Now();
        for (i = 0U; i < (uint8_t)AXIS_COUNT; i++) {
            if (Sensors_HomeLevel((AxisId)i)) {
                s_ctx.home_latched[i] = true;
            }
        }
    }
}

/* ==========================================================================
 * IR 공개 인터페이스
 * ========================================================================== */

void Sensors_IrClear(void)
{
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        s_ctx.ir_latched = false;
    }
}

bool Sensors_IrDetected(void)
{
    bool v;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        v = s_ctx.ir_latched;
    }
    return v;
}

void Sensors_IrEnable(bool en)
{
    if (en) {
        /* 닫혀 있는 동안 쌓인 플래그를 먼저 지우지 않으면
         * 여는 즉시 과거 엣지로 ISR 이 한 번 돈다. */
        EIFR   = (uint8_t)((1U << INTF2) | (1U << INTF4));
        EIMSK |= (uint8_t)((1U << INT2) | (1U << INT4));
    } else {
        EIMSK &= (uint8_t)~((1U << INT2) | (1U << INT4));
    }
    /* 상태는 EIMSK 가 원본이므로 별도 미러 변수를 두지 않는다. */
}

/* ==========================================================================
 * 홈 센서 공개 인터페이스
 * ========================================================================== */

bool Sensors_HomeLevel(AxisId axis)
{
    uint8_t lvl;

    if (axis == AXIS_X) {
        lvl = (HOME_X_PIN_REG & (uint8_t)(1U << HOME_X_BIT)) ? 1U : 0U;
    } else if (axis == AXIS_Y) {
        lvl = (HOME_Y_PIN_REG & (uint8_t)(1U << HOME_Y_BIT)) ? 1U : 0U;
    } else {
        return false;
    }

    return (lvl == HOME_ACTIVE_LEVEL);
}

bool Sensors_HomeLatched(AxisId axis)
{
    bool v;

    if ((uint8_t)axis >= (uint8_t)AXIS_COUNT) {
        return false;
    }

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        v = s_ctx.home_latched[axis];
    }
    return v;
}

void Sensors_HomeClear(AxisId axis)
{
    if ((uint8_t)axis >= (uint8_t)AXIS_COUNT) {
        return;
    }

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        s_ctx.home_latched[axis] = false;
    }
}

/* ==========================================================================
 * ISR
 * --------------------------------------------------------------------------
 * ISR 은 전부 "플래그 세트" 한 줄만 한다.
 * 판정 로직을 넣으면 스테퍼 3ms 틱 타이밍을 흔든다.
 * ========================================================================== */

/** 홈 X (PD0) rising edge */
ISR(INT0_vect)
{
    s_ctx.home_latched[AXIS_X] = true;
}

/** 홈 Y (PD1) rising edge */
ISR(INT1_vect)
{
    s_ctx.home_latched[AXIS_Y] = true;
}

/** IR1 (PD2) falling edge — 설계서 §6: 감지 즉시 래치 (홈 센서와 동일 방식) */
ISR(INT2_vect)
{
    s_ctx.ir_latched = true;
}

/** IR2 (PE4) falling edge — OR 로직: IR1/IR2 어느 쪽이든 같은 래치 */
ISR(INT4_vect)
{
    s_ctx.ir_latched = true;
}
