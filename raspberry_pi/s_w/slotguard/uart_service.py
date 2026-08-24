import math
import re
import threading
import time
from datetime import datetime, timedelta

from database import (
    ACTIVE_STATUSES,
    ActiveDoseError,
    MAX_ALLOWED_SECONDS,
    assign_coordinate,
    complete_dispensed,
    continue_after_empty_slot,
    get_active_schedule,
    get_current_coordinate,
    get_device_state,
    get_due_schedule,
    get_schedule,
    get_schedule_by_dispense_request,
    get_unacknowledged_result,
    is_result_request_processed,
    mark_comm_error,
    mark_dispense_ack,
    mark_failed,
    mark_missed,
    mark_move_ack,
    mark_ready,
    prepare_dispense,
    prepare_move,
    record_dispense_sent,
    record_move_sent,
    resume_after_empty_blister,
)

try:
    import serial
except ImportError:
    serial = None


UART_PORT = "/dev/serial0"
UART_BAUD_RATE = 9600
ACK_RETRY_SECONDS = 10
RECONNECT_SECONDS = 5

REQUEST_ID_PATTERN = re.compile(r"^[0-9A-F]{8}$")
RESULT_PATTERN = re.compile(r"^([01])([0-4])([012])$")
ERROR_CODE_PATTERN = re.compile(r"^[A-Z0-9_-]{1,40}$")


def _validate_request_id(request_id):
    if not isinstance(request_id, str) or not REQUEST_ID_PATTERN.fullmatch(
        request_id
    ):
        raise ValueError("request_id must be 8 uppercase hexadecimal characters")


def _validate_coordinate(x_coordinate, y_coordinate):
    if x_coordinate not in (0, 1):
        raise ValueError("x_coordinate must be 0 or 1")
    if y_coordinate not in range(5):
        raise ValueError("y_coordinate must be between 0 and 4")


def build_move_command(
    request_id,
    x_coordinate,
    y_coordinate,
    allowed_seconds,
):
    _validate_request_id(request_id)
    _validate_coordinate(x_coordinate, y_coordinate)
    if allowed_seconds < 1 or allowed_seconds > MAX_ALLOWED_SECONDS:
        raise ValueError(
            "allowed_seconds must be between "
            f"1 and {MAX_ALLOWED_SECONDS}"
        )
    return (
        f"MOVE|{request_id}|{x_coordinate}|{y_coordinate}|"
        f"{allowed_seconds:06d}\n"
    ).encode("ascii")


def build_dispense_command(request_id, x_coordinate, y_coordinate):
    _validate_request_id(request_id)
    _validate_coordinate(x_coordinate, y_coordinate)
    return (
        f"DISPENSE|{request_id}|{x_coordinate}|{y_coordinate}\n"
    ).encode("ascii")


def build_result_ack(request_id):
    _validate_request_id(request_id)
    return f"ACK|{request_id}|RESULT\n".encode("ascii")


def parse_result_message(message):
    match = RESULT_PATTERN.fullmatch(message)
    if not match:
        return None
    return tuple(int(value) for value in match.groups())


def parse_protocol_message(message):
    parts = message.split("|")

    if len(parts) == 3 and parts[0] == "ACK":
        request_id, action = parts[1], parts[2]
        if (
            REQUEST_ID_PATTERN.fullmatch(request_id)
            and action in {"MOVE", "DISPENSE", "RESULT"}
        ):
            return {
                "type": "ACK",
                "request_id": request_id,
                "action": action,
            }

    if len(parts) == 2 and parts[0] == "WAIT":
        request_id = parts[1]
        if REQUEST_ID_PATTERN.fullmatch(request_id):
            return {"type": "WAIT", "request_id": request_id}

    if len(parts) == 3 and parts[0] == "RESULT":
        request_id, payload = parts[1], parts[2]
        result = parse_result_message(payload)
        if REQUEST_ID_PATTERN.fullmatch(request_id) and result is not None:
            return {
                "type": "RESULT",
                "request_id": request_id,
                "result": result,
            }

    if len(parts) == 3 and parts[0] == "ERROR":
        request_id, error_code = parts[1], parts[2]
        if (
            REQUEST_ID_PATTERN.fullmatch(request_id)
            and ERROR_CODE_PATTERN.fullmatch(error_code)
        ):
            return {
                "type": "ERROR",
                "request_id": request_id,
                "error_code": error_code,
            }

    # Temporary compatibility while the ATmega team migrates to protocol v2.
    if message == "ACK":
        return {"type": "LEGACY_ACK"}
    if message == "WAIT":
        return {"type": "LEGACY_WAIT"}
    legacy_result = parse_result_message(message)
    if legacy_result is not None:
        return {"type": "LEGACY_RESULT", "result": legacy_result}

    return None


class UartService:
    def __init__(
        self,
        port=UART_PORT,
        baud_rate=UART_BAUD_RATE,
        ack_retry_seconds=ACK_RETRY_SECONDS,
        on_schedule_started=None,
        on_alert_stop=None,
        on_blister_exhausted=None,
        is_time_ready=None,
    ):
        self.port = port
        self.baud_rate = baud_rate
        self.ack_retry_seconds = ack_retry_seconds
        self.on_schedule_started = on_schedule_started
        self.on_alert_stop = on_alert_stop
        self.on_blister_exhausted = on_blister_exhausted
        self.is_time_ready = is_time_ready

        self._serial = None
        self._thread = None
        self._stop_event = threading.Event()
        self._state_lock = threading.Lock()
        self._serial_lock = threading.Lock()

        self._active_schedule_id = None
        self._last_move_transmit_at = None
        self._last_dispense_transmit_at = None
        self._next_reconnect_at = 0
        self._last_schedule_check_at = 0
        self._receive_buffer = bytearray()

        self._connection_state = "STOPPED"
        self._last_message = None
        self._last_error = None

    def start(self):
        if self._thread and self._thread.is_alive():
            return

        self._stop_event.clear()
        self._connection_state = (
            "PYSERIAL_MISSING" if serial is None else "DISCONNECTED"
        )
        self._thread = threading.Thread(
            target=self._run,
            name="slotguard-uart",
            daemon=True,
        )
        self._thread.start()

    def stop(self):
        self._stop_event.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=2)
        self._disconnect()
        self._connection_state = "STOPPED"

    def reset_runtime_state(self):
        with self._state_lock:
            self._active_schedule_id = None
            self._last_move_transmit_at = None
            self._last_dispense_transmit_at = None

    def cancel_schedule(self, schedule_id):
        with self._state_lock:
            if self._active_schedule_id != schedule_id:
                return False
            self._active_schedule_id = None
            self._last_move_transmit_at = None
            self._last_dispense_transmit_at = None
            return True

    def request_dispense(self, schedule_id):
        schedule = get_schedule(schedule_id)
        if schedule is None or schedule["status"] != "READY_TO_DISPENSE":
            raise ValueError("약 배출 준비 상태가 아닙니다.")

        now = datetime.now()
        if now >= self._deadline_for(schedule):
            mark_missed(
                schedule_id,
                self._format_time(now),
                "DOSE_BUTTON_TIMEOUT",
            )
            self._run_callback(self.on_alert_stop)
            self._clear_active_schedule()
            raise ValueError("복약 허용시간이 종료되었습니다.")

        schedule = prepare_dispense(schedule_id, self._format_time(now))
        with self._state_lock:
            self._active_schedule_id = schedule_id
            self._last_dispense_transmit_at = None
        self._run_callback(self.on_alert_stop)
        self._transmit_dispense(schedule, force=True)
        return get_schedule(schedule_id)

    def resume_empty_blister_dose(self, schedule_id):
        schedule = get_schedule(schedule_id)
        if (
            schedule is None
            or schedule["status"] != "FAILED"
            or schedule["error_code"] != "EMPTY_BLISTER_SLOT"
            or schedule["acknowledged_at"] is not None
        ):
            raise ActiveDoseError("재개할 빈 슬롯 복약 기록이 없습니다.")

        now = datetime.now()
        now_text = self._format_time(now)
        if now >= self._deadline_for(schedule):
            mark_missed(schedule_id, now_text, "DOSE_WINDOW_EXPIRED")
            self._run_callback(self.on_alert_stop)
            self._clear_active_schedule()
            return get_schedule(schedule_id)

        remaining_seconds = self._remaining_seconds(schedule, now)
        schedule = resume_after_empty_blister(
            schedule_id,
            now_text,
            remaining_seconds,
        )
        with self._state_lock:
            self._active_schedule_id = schedule_id
            self._last_move_transmit_at = None
            self._last_dispense_transmit_at = None
        self._transmit_move(schedule, force=True)
        return get_schedule(schedule_id)

    def get_status(self):
        with self._state_lock:
            return {
                "uart": self._connection_state,
                "port": self.port,
                "baud_rate": self.baud_rate,
                "protocol": "SLOT-GUARD-UART-v2",
                "active_schedule_id": self._active_schedule_id,
                "last_message": self._last_message,
                "last_error": self._last_error,
            }

    def _run(self):
        while not self._stop_event.is_set():
            try:
                self._ensure_connection()
                self._read_available_lines()
                self._process_schedules()
            except Exception as error:
                self._set_error(f"UART 작업 오류: {error}")
            self._stop_event.wait(0.2)

    def _ensure_connection(self):
        if serial is None or self._serial is not None:
            return
        current_time = time.monotonic()
        if current_time < self._next_reconnect_at:
            return
        self._next_reconnect_at = current_time + RECONNECT_SECONDS

        try:
            self._serial = serial.Serial(
                port=self.port,
                baudrate=self.baud_rate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=0.2,
                write_timeout=1,
            )
            self._serial.reset_input_buffer()
            self._receive_buffer.clear()
            self._connection_state = "CONNECTED"
            self._last_error = None
            print(
                f"[UART] 연결됨: {self.port}, "
                f"{self.baud_rate} baud, 8N1"
            )
        except (OSError, serial.SerialException) as error:
            self._serial = None
            self._connection_state = "DISCONNECTED"
            self._set_error(f"{self.port} 연결 실패: {error}")

    def _disconnect(self, error=None):
        with self._serial_lock:
            if self._serial is not None:
                try:
                    self._serial.close()
                except (OSError, serial.SerialException):
                    pass
            self._serial = None

        self._receive_buffer.clear()
        if serial is not None:
            self._connection_state = "DISCONNECTED"
        if error:
            self._set_error(str(error))

    def _read_available_lines(self):
        if self._serial is None:
            return

        try:
            waiting = self._serial.in_waiting
            if waiting <= 0:
                return
            self._receive_buffer.extend(self._serial.read(waiting))

            while b"\n" in self._receive_buffer:
                raw_message, _, remainder = self._receive_buffer.partition(b"\n")
                self._receive_buffer = bytearray(remainder)
                raw_message = raw_message.rstrip(b"\r")
                try:
                    message = raw_message.decode("ascii")
                except UnicodeDecodeError:
                    self._set_error(
                        f"ASCII가 아닌 UART 데이터: {raw_message!r}"
                    )
                    continue
                if message:
                    self._handle_message(message)

            if len(self._receive_buffer) > 256:
                self._set_error("UART 수신 버퍼가 256바이트를 초과했습니다.")
                self._receive_buffer.clear()
        except (OSError, serial.SerialException) as error:
            self._disconnect(f"UART 수신 실패: {error}")

    def _handle_message(self, message):
        self._last_message = message
        print(f"[UART] AT → Pi: {message!r}")
        parsed = parse_protocol_message(message)
        if parsed is None:
            self._set_error(f"알 수 없는 UART 메시지: {message!r}")
            return

        message_type = parsed["type"]
        if message_type == "ACK":
            self._handle_ack(parsed["request_id"], parsed["action"])
        elif message_type == "WAIT":
            self._handle_wait(parsed["request_id"])
        elif message_type == "RESULT":
            self._handle_result(parsed["request_id"], *parsed["result"])
        elif message_type == "ERROR":
            self._handle_device_error(
                parsed["request_id"],
                parsed["error_code"],
            )
        else:
            self._handle_legacy(parsed)

    def _handle_ack(self, request_id, action):
        schedule = self._get_active_schedule()
        if schedule is None:
            return
        event_time = self._format_time(datetime.now())

        if action == "MOVE" and request_id == schedule["move_request_id"]:
            mark_move_ack(schedule["id"], request_id, event_time)
        elif (
            action == "DISPENSE"
            and request_id == schedule["dispense_request_id"]
        ):
            mark_dispense_ack(schedule["id"], request_id, event_time)
        elif action != "RESULT":
            self._set_error(
                "ACK 요청번호 또는 단계 불일치: "
                f"request_id={request_id}, action={action}"
            )

    def _handle_wait(self, request_id):
        schedule = self._get_active_schedule()
        if schedule is None:
            return
        if request_id != schedule["move_request_id"]:
            self._set_error(
                f"WAIT 요청번호 불일치: 예상={schedule['move_request_id']}, "
                f"수신={request_id}"
            )
            return
        mark_ready(
            schedule["id"],
            request_id,
            self._format_time(datetime.now()),
        )

    def _handle_result(
        self,
        request_id,
        x_coordinate,
        y_coordinate,
        result_code,
    ):
        schedule = self._get_active_schedule()
        if schedule is None:
            previous = get_schedule_by_dispense_request(request_id)
            if (
                previous
                and previous["status"]
                in {"DISPENSED", "FAILED", "MANUALLY_COMPLETED"}
            ) or is_result_request_processed(request_id):
                self._write(build_result_ack(request_id))
            return

        if request_id != schedule["dispense_request_id"]:
            if is_result_request_processed(request_id):
                self._write(build_result_ack(request_id))
                return
            self._set_error(
                "RESULT 요청번호 불일치: "
                f"예상={schedule['dispense_request_id']}, 수신={request_id}"
            )
            return

        expected_coordinate = (
            schedule["x_coordinate"],
            schedule["y_coordinate"],
        )
        received_coordinate = (x_coordinate, y_coordinate)
        if received_coordinate != expected_coordinate:
            self._set_error(
                "결과 좌표 불일치: "
                f"예상={expected_coordinate}, 수신={received_coordinate}"
            )
            return

        now = datetime.now()
        event_time = self._format_time(now)
        mark_dispense_ack(schedule["id"], request_id, event_time)
        if result_code == 1:
            coordinate_result = complete_dispensed(
                schedule["id"],
                request_id,
                event_time,
            )
            if coordinate_result and coordinate_result["blister_exhausted"]:
                self._run_callback(self.on_blister_exhausted)
        elif result_code == 0:
            mark_failed(
                schedule["id"],
                request_id,
                event_time,
                "NO_DROP_DETECTED",
            )
        else:
            remaining_seconds = self._remaining_seconds(schedule, now)
            coordinate_result = continue_after_empty_slot(
                schedule["id"],
                request_id,
                event_time,
                remaining_seconds,
            )

        self._write(build_result_ack(request_id))
        if result_code == 2 and coordinate_result is not None:
            if coordinate_result["blister_exhausted"]:
                self._run_callback(self.on_blister_exhausted)
                self._clear_active_schedule()
            else:
                with self._state_lock:
                    self._last_move_transmit_at = None
                    self._last_dispense_transmit_at = None
                self._transmit_move(
                    coordinate_result["schedule"],
                    force=True,
                )
            return

        self._run_callback(self.on_alert_stop)
        self._clear_active_schedule()

    def _handle_device_error(self, request_id, error_code):
        schedule = self._get_active_schedule()
        if schedule is None:
            return
        if request_id not in {
            schedule["move_request_id"],
            schedule["dispense_request_id"],
        }:
            self._set_error(f"ERROR 요청번호 불일치: {request_id}")
            return
        mark_comm_error(
            schedule["id"],
            self._format_time(datetime.now()),
            f"AT_{error_code}",
        )
        self._run_callback(self.on_alert_stop)
        self._clear_active_schedule()

    def _handle_legacy(self, parsed):
        schedule = self._get_active_schedule()
        if schedule is None:
            return
        event_time = self._format_time(datetime.now())
        message_type = parsed["type"]
        self._set_error(
            "ATmega가 요청번호 없는 구형 프로토콜을 사용 중입니다."
        )

        if message_type == "LEGACY_ACK":
            if schedule["status"] == "MOVING":
                mark_move_ack(
                    schedule["id"], schedule["move_request_id"], event_time
                )
            elif schedule["status"] == "DISPENSING":
                mark_dispense_ack(
                    schedule["id"],
                    schedule["dispense_request_id"],
                    event_time,
                )
        elif message_type == "LEGACY_WAIT" and schedule["status"] == "MOVING":
            mark_ready(
                schedule["id"], schedule["move_request_id"], event_time
            )
        elif message_type == "LEGACY_RESULT":
            self._handle_result(
                schedule["dispense_request_id"],
                *parsed["result"],
            )

    def _process_schedules(self):
        current_monotonic = time.monotonic()
        if current_monotonic - self._last_schedule_check_at < 0.5:
            return
        self._last_schedule_check_at = current_monotonic

        if not self._system_time_is_ready():
            return
        if get_unacknowledged_result() is not None:
            return

        now = datetime.now()
        now_text = self._format_time(now)
        schedule = self._get_active_schedule()

        if schedule is None:
            schedule = get_active_schedule()
            if schedule is None:
                if get_device_state()["blister_exhausted"]:
                    return
                schedule = get_due_schedule(now.strftime("%Y-%m-%d %H:%M"))
                if schedule is not None:
                    if now >= self._deadline_for(schedule):
                        mark_missed(
                            schedule["id"],
                            now_text,
                            "DOSE_WINDOW_EXPIRED",
                        )
                        return
                    x_coordinate, y_coordinate = get_current_coordinate()
                    schedule = assign_coordinate(
                        schedule["id"], x_coordinate, y_coordinate
                    )
                    schedule = prepare_move(schedule["id"], now_text)
                    self._run_callback(self.on_schedule_started)

            if schedule is not None:
                with self._state_lock:
                    self._active_schedule_id = schedule["id"]
                    self._last_move_transmit_at = None
                    self._last_dispense_transmit_at = None

        if schedule is None:
            return

        if now >= self._deadline_for(schedule):
            self._expire_active_schedule(schedule, now_text)
            return

        if schedule["status"] == "MOVING":
            if schedule["move_ack_at"] is None:
                self._transmit_move(schedule)
        elif schedule["status"] == "DISPENSING":
            if schedule["dispense_ack_at"] is None:
                self._transmit_dispense(schedule)

    def _expire_active_schedule(self, schedule, event_time):
        if schedule["status"] == "READY_TO_DISPENSE":
            mark_missed(schedule["id"], event_time, "DOSE_BUTTON_TIMEOUT")
        elif schedule["status"] == "MOVING":
            error_code = (
                "MOVE_ACK_TIMEOUT"
                if schedule["move_ack_at"] is None
                else "MOVE_READY_TIMEOUT"
            )
            mark_comm_error(schedule["id"], event_time, error_code)
        elif schedule["status"] == "DISPENSING":
            error_code = (
                "DISPENSE_ACK_TIMEOUT"
                if schedule["dispense_ack_at"] is None
                else "RESULT_TIMEOUT"
            )
            mark_comm_error(schedule["id"], event_time, error_code)
        self._run_callback(self.on_alert_stop)
        self._clear_active_schedule()

    def _transmit_move(self, schedule, force=False):
        if not self._transmit_due(self._last_move_transmit_at, force):
            return
        payload = build_move_command(
            schedule["move_request_id"],
            schedule["x_coordinate"],
            schedule["y_coordinate"],
            schedule["move_allowed_seconds"] or schedule["allowed_seconds"],
        )
        if self._write(payload):
            record_move_sent(schedule["id"], self._format_time(datetime.now()))
            self._last_move_transmit_at = time.monotonic()

    def _transmit_dispense(self, schedule, force=False):
        if not self._transmit_due(self._last_dispense_transmit_at, force):
            return
        payload = build_dispense_command(
            schedule["dispense_request_id"],
            schedule["x_coordinate"],
            schedule["y_coordinate"],
        )
        if self._write(payload):
            record_dispense_sent(
                schedule["id"], self._format_time(datetime.now())
            )
            self._last_dispense_transmit_at = time.monotonic()

    def _transmit_due(self, last_transmit_at, force):
        if self._serial is None:
            return False
        if force or last_transmit_at is None:
            return True
        return time.monotonic() - last_transmit_at >= self.ack_retry_seconds

    def _write(self, payload):
        if self._serial is None:
            return False
        try:
            with self._serial_lock:
                self._serial.write(payload)
                self._serial.flush()
            print(f"[UART] Pi → AT: {payload.decode('ascii').strip()!r}")
            return True
        except (OSError, serial.SerialException) as error:
            self._disconnect(f"UART 송신 실패: {error}")
            return False

    def _get_active_schedule(self):
        with self._state_lock:
            schedule_id = self._active_schedule_id
        if schedule_id is None:
            return None
        schedule = get_schedule(schedule_id)
        if schedule is None or schedule["status"] not in ACTIVE_STATUSES:
            self._clear_active_schedule()
            return None
        return schedule

    def _clear_active_schedule(self):
        with self._state_lock:
            self._active_schedule_id = None
            self._last_move_transmit_at = None
            self._last_dispense_transmit_at = None

    @staticmethod
    def _deadline_for(schedule):
        scheduled_at = datetime.strptime(
            schedule["scheduled_at"], "%Y-%m-%d %H:%M"
        )
        return scheduled_at + timedelta(seconds=schedule["allowed_seconds"])

    @classmethod
    def _remaining_seconds(cls, schedule, now):
        remaining = math.ceil((cls._deadline_for(schedule) - now).total_seconds())
        return max(1, min(MAX_ALLOWED_SECONDS, remaining))

    @staticmethod
    def _format_time(value):
        return value.strftime("%Y-%m-%d %H:%M:%S")

    def _run_callback(self, callback):
        if callback is None:
            return
        try:
            callback()
        except Exception as error:
            self._set_error(f"콜백 실행 실패: {error}")

    def _system_time_is_ready(self):
        if self.is_time_ready is None:
            return True
        try:
            return bool(self.is_time_ready())
        except Exception as error:
            self._set_error(f"시스템 시간 상태 확인 실패: {error}")
            return False

    def _set_error(self, message):
        if message != self._last_error:
            print(f"[UART] {message}")
        self._last_error = message
