// =====================================================================
// protocol.c
// =====================================================================

#include <string.h>
#include <stdio.h>
#include "protocol.h"
#include "uart.h"
#include "config.h"

#define FRAME_MAX_CONTENT_LEN (FRAME_MAX_LEN - 1U)

// 대문자/숫자/구분자(|)만 허용
static bool IsUpperAsciiOrDigitOrDelim(char c)
{
    if (c >= 'A' && c <= 'Z') return true;		//대문자
    if (c >= '0' && c <= '9') return true;		//숫자
    if (c == FRAME_DELIM) return true;			//구분자 |
    return false;
}

// 구분자(|)를 기준으로 문자열을 최대 max_fields개 필드로 분리 (반환값: 분리된 필드 개수)
static uint8_t SplitFields(char *work, char *fields[], uint8_t max_fields)
{
    uint8_t count = 0;
    char *p = work;

    fields[count++] = p; //첫 필드 시작 위치
    while (*p != '\0') {
        if (*p == FRAME_DELIM) {
            *p = '\0'; //|를 \0로 바꿔 필드 분리
            p++;
            if (count >= max_fields) return 0; //허용 필드 수 초과 시 실패
            fields[count++] = p; //다음 필드의 시작 주소 저장
        } else {
            p++;
        }
    }
    return count; //분리된 필드 개수를 반환
}

// REQ_ID 파싱: "12AB34CD" -> 0x12AB34CD
// 반환값은 실제로 파싱에 성공한 hex 자릿수(정상이면 REQ_ID_HEX_LEN)
uint8_t Protocol_ParseHex32(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    uint8_t  i;

    for (i = 0; i < REQ_ID_HEX_LEN; i++) {
        char c = s[i];
        uint8_t nibble;

        if (c >= '0' && c <= '9') {
            nibble = (uint8_t)(c - '0'); //'0'~'9'를 16진수로 변환
        } else if (c >= 'A' && c <= 'F') {
            nibble = (uint8_t)(c - 'A' + 10); //대문자 'A'~'F'를 16진수로 변환
        } else {
            break; //허용되지 않은 문자 -> 중단
        }
        v = (v << 4) | nibble; //HEX 숫자를 한 자리씩 누적
    }
    if (i == REQ_ID_HEX_LEN) {
        *out = v;
    }
    return i;
}

// 지정 길이(len)의 십진수 문자열을 uint32_t로 변환: "123" -> 123
static bool ParseDecimal(const char *s, uint8_t len, uint32_t *out)
{
    uint32_t v = 0;
    uint8_t  i;

    if (len == 0) return false;
    for (i = 0; i < len; i++) {
        char c = s[i];
        if (c < '0' || c > '9') return false; //숫자가 아니면 파싱 실패
        v = (v * 10U) + (uint32_t)(c - '0'); //십진수 자리 누적
    }
    *out = v;
    return true;
}

//출력 구조체를 초기 상태(파싱 실패 취급)로 되돌림
static void FrameInvalidate(Frame *out)
{
    memset(out, 0, sizeof(*out));
    out->cmd   = CMD_UNKNOWN;
    out->valid = false;
}


//line을 파싱해 Frame 구조체에 저장
bool Protocol_Parse(const char *line, Frame *out)
{
    char     work[FRAME_MAX_CONTENT_LEN + 1U];
    char    *fields[FRAME_MAX_FIELDS];
    uint8_t  nfields;
    uint8_t  len;
    uint8_t  i;

    FrameInvalidate(out); //출력 구조체를 초기 상태로 초기화

    len = (uint8_t)strlen(line); //입력 문자열 길이 확인
    if (len == 0 || len > FRAME_MAX_CONTENT_LEN) return false; //비었거나 최대 길이 초과 시 실패

    for (i = 0; i < len; i++) {
        if (!IsUpperAsciiOrDigitOrDelim(line[i])) return false; //허용되지 않은 문자 포함 시 실패
    }

    memcpy(work, line, (size_t)len + 1U); //문자열을 work로 복사
    nfields = SplitFields(work, fields, FRAME_MAX_FIELDS); //구분자를 기준으로 분리
    if (nfields == 0) return false; //필드 과다/파싱 실패

    //*************MOVE|<REQ_ID>|<X>|<Y>|<ALLOW_TIME>***************
    if (strcmp(fields[0], TOK_MOVE) == 0) {
        uint32_t req_id, xv, yv, tv;
        uint8_t  tlen;

        if (nfields != 5) return false; //필드 개수 확인

        //<REQ_ID>
        if (Protocol_ParseHex32(fields[1], &req_id) != REQ_ID_HEX_LEN) return false;
        if (fields[1][REQ_ID_HEX_LEN] != '\0') return false; //8자리보다 길면 실패
        if (req_id == REQ_ID_NONE) return false; //예약된 ID(0x00000000)는 사용 불가

        if (!ParseDecimal(fields[2], (uint8_t)strlen(fields[2]), &xv)) return false;
        if (!ParseDecimal(fields[3], (uint8_t)strlen(fields[3]), &yv)) return false;
        if (xv > 0xFFU || yv > 0xFFU) return false;

        //<ALLOW_TIME> — TTTTTT는 정확히 6자리
        tlen = (uint8_t)strlen(fields[4]);
        if (tlen != 6U) return false;
        if (!ParseDecimal(fields[4], tlen, &tv)) return false;

        out->cmd = CMD_MOVE;
        out->req_id = req_id;
        out->x = (uint8_t)xv;
        out->y = (uint8_t)yv;
        out->allow_time_sec = tv;
        out->valid = true; //검증 성공
        return true;
    }

    //************DISPENSE|<REQ_ID>|<X>|<Y>**************
    if (strcmp(fields[0], TOK_DISPENSE) == 0) {
        uint32_t req_id, xv, yv;

        if (nfields != 4) return false; //필드 개수 확인

        //<REQ_ID>
        if (Protocol_ParseHex32(fields[1], &req_id) != REQ_ID_HEX_LEN) return false;
        if (fields[1][REQ_ID_HEX_LEN] != '\0') return false;
        if (req_id == REQ_ID_NONE) return false;

        //<X>, <Y> — MOVE와 동일하게 구조만 확인, 그리드 범위는 fsm.c에서
        if (!ParseDecimal(fields[2], (uint8_t)strlen(fields[2]), &xv)) return false;
        if (!ParseDecimal(fields[3], (uint8_t)strlen(fields[3]), &yv)) return false;
        if (xv > 0xFFU || yv > 0xFFU) return false;

        out->cmd    = CMD_DISPENSE;
        out->req_id = req_id;
        out->x      = (uint8_t)xv;
        out->y      = (uint8_t)yv;
        out->valid  = true; //검증 성공
        return true;
    }

    //************ACK|<REQ_ID>|<RESULT>**************
    if (strcmp(fields[0], TOK_ACK) == 0) {
        uint32_t req_id;

        if (nfields != 3) return false; //필드 개수 확인

        //<REQ_ID>
        if (Protocol_ParseHex32(fields[1], &req_id) != REQ_ID_HEX_LEN) return false;
        if (fields[1][REQ_ID_HEX_LEN] != '\0') return false;
        if (req_id == REQ_ID_NONE) return false;

        //<RESULT> — Pi가 보내는 ACK는 RESULT 수신 확인 용도로만 쓰임
        if (strcmp(fields[2], TOK_RESULT) != 0) return false;

        out->cmd    = CMD_ACK;
        out->req_id = req_id;
        strcpy(out->ack_target, fields[2]); //ACK 대상 문자열 저장
        out->valid  = true; //검증 성공
        return true;
    }

    //************TIMEOUT|<REQ_ID>**************
    if (strcmp(fields[0], TOK_TIMEOUT) == 0) {
        uint32_t req_id;

        if (nfields != 2) return false; //필드 개수 확인

        //<REQ_ID>
        if (Protocol_ParseHex32(fields[1], &req_id) != REQ_ID_HEX_LEN) return false;
        if (fields[1][REQ_ID_HEX_LEN] != '\0') return false;
        if (req_id == REQ_ID_NONE) return false;

        out->cmd    = CMD_TIMEOUT;
        out->req_id = req_id;
        out->valid  = true; //검증 성공
        return true;
    }

    return false; //나머지는 정의되지 않은 명령어
}

//송신
void Protocol_SendAck(uint32_t req_id, const char *target) // "ACK|0000000F|DISPENSE\n"
{
    char buf[TX_FRAME_BUF_LEN];
    // %08lX: req_id를 8자리 16진수 대문자로 변환 (앞쪽 빈자리는 0으로 채움)
    snprintf(buf, sizeof(buf), "%s|%08lX|%s", TOK_ACK, (unsigned long)req_id, target);
    Uart_WriteLine(buf);
}

void Protocol_SendWait(uint32_t req_id) // "WAIT|0000000F\n"
{
    char buf[TX_FRAME_BUF_LEN];
    snprintf(buf, sizeof(buf), "%s|%08lX", TOK_WAIT, (unsigned long)req_id);
    Uart_WriteLine(buf);
}

void Protocol_SendResult(uint32_t req_id, uint8_t x, uint8_t y, DispenseResult r) // "RESULT|0000000F|121\n"
{
    char buf[TX_FRAME_BUF_LEN];
    snprintf(buf, sizeof(buf), "%s|%08lX|%u%u%u", TOK_RESULT, (unsigned long)req_id,
             (unsigned int)(x % 10U), (unsigned int)(y % 10U), (unsigned int)r);
    Uart_WriteLine(buf);
}

void Protocol_SendError(uint32_t req_id, ErrorCode code) // "ERROR|0000000F|BUSY\n"
{
    char buf[TX_FRAME_BUF_LEN];
    snprintf(buf, sizeof(buf), "%s|%08lX|%s", TOK_ERROR, (unsigned long)req_id, Protocol_ErrorText(code));
    Uart_WriteLine(buf);
}

void Protocol_SendInvalidFormat(void) // "ERROR|INVALID_FORMAT\n"
{
    char buf[TX_FRAME_BUF_LEN];
    snprintf(buf, sizeof(buf), "%s|%s", TOK_ERROR, Protocol_ErrorText(ERR_INVALID_FORMAT));
    Uart_WriteLine(buf);
}

const char *Protocol_ErrorText(ErrorCode code)
{
    switch (code) {
        case ERR_INVALID_FORMAT:     return "INVALID_FORMAT";
        case ERR_INVALID_COORD:      return "INVALID_COORD";
        case ERR_INVALID_TIME:       return "INVALID_TIME";
        case ERR_ID_CONFLICT:        return "ID_CONFLICT";
        case ERR_BUSY:                return "BUSY";
        case ERR_NOT_READY:          return "NOT_READY";
        case ERR_COORD_MISMATCH:     return "COORD_MISMATCH";
        case ERR_RECOVERY_REQUIRED:  return "RECOVERY_REQUIRED";
        case ERR_STEPPER_ERROR:      return "STEPPER_ERROR";
        case ERR_SERVO_ERROR:        return "SERVO_ERROR";
        case ERR_SENSOR_ERROR:       return "SENSOR_ERROR";
        default:                      return "INVALID_FORMAT";
    }
}
