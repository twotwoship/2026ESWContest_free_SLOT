import re
import threading
import time
from datetime import datetime, timedelta

from database import (
    MAX_ALLOWED_SECONDS,
    assign_coordinate,
    complete_schedule_success,
    expire_unstarted_schedules,
    get_active_schedule,
    get_current_coordinate,
    get_due_schedule,
    get_schedule,
    mark_synced,
    record_command_sent,
    update_status,
)

try:
    import serial
except ImportError:
    serial = None


UART_PORT = "/dev/serial0"
UART_BAUD_RATE = 9600
ACK_RETRY_SECONDS = 10
RECONNECT_SECONDS = 5
ACK_MESSAGE = b"ACK\n"

RESULT_PATTERN = re.compile(r"^([01])([0-4])([01])$")


def build_schedule_command(x_coordinate, y_coordinate, allowed_seconds):
    if x_coordinate not in (0, 1):
        raise ValueError("x_coordinate must be 0 or 1")

    if y_coordinate not in range(5):
        raise ValueError("y_coordinate must be between 0 and 4")

    if allowed_seconds < 1 or allowed_seconds > MAX_ALLOWED_SECONDS:
        raise ValueError(
            "allowed_seconds must be between "
            f"1 and {MAX_ALLOWED_SECONDS}"
        )

    return (
        f"{x_coordinate}{y_coordinate}"
        f"{allowed_seconds:06d}\n"
    ).encode("ascii")


def parse_result_message(message):
    match = RESULT_PATTERN.fullmatch(message)

    if not match:
        return None

    return tuple(int(value) for value in match.groups())


class UartService:
    def __init__(
        self,
        port=UART_PORT,
        baud_rate=UART_BAUD_RATE,
        ack_retry_seconds=ACK_RETRY_SECONDS,
        on_schedule_started=None,
        on_blister_exhausted=None,
        is_time_ready=None,
    ):
        self.port = port
        self.baud_rate = baud_rate
        self.ack_retry_seconds = ack_retry_seconds
        self.on_schedule_started = on_schedule_started
        self.on_blister_exhausted = on_blister_exhausted
        self.is_time_ready = is_time_ready

        self._serial = None
        self._thread = None
        self._stop_event = threading.Event()
        self._state_lock = threading.Lock()

        self._active_schedule_id = None
        self._last_transmit_at = None
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
            self._last_transmit_at = None

    def cancel_schedule(self, schedule_id):
        with self._state_lock:
            if self._active_schedule_id != schedule_id:
                return False

            self._active_schedule_id = None
            self._last_transmit_at = None
            return True

    def get_status(self):
        with self._state_lock:
            return {
                "uart": self._connection_state,
                "port": self.port,
                "baud_rate": self.baud_rate,
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
                raw_message, _, remainder = self._receive_buffer.partition(
                    b"\n"
                )
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

        if message == "ACK":
            self._handle_command_ack()
            return

        if message == "WAIT":
            self._handle_command_ack()
            return

        result = parse_result_message(message)

        if result is None:
            self._set_error(f"알 수 없는 UART 메시지: {message!r}")
            return

        self._handle_dispense_result(*result)
        self._write(ACK_MESSAGE)

    def _handle_command_ack(self):
        schedule = self._get_active_schedule()

        if schedule is None:
            return

        if schedule["status"] == "WAITING":
            mark_synced(
                schedule["id"],
                datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            )

    def _handle_dispense_result(
        self,
        x_coordinate,
        y_coordinate,
        succeeded,
    ):
        schedule = self._get_active_schedule()

        if schedule is None:
            print(
                "[UART] 처리 중인 일정이 없어 결과를 확인만 했습니다."
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

        event_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

        if schedule["status"] == "WAITING":
            mark_synced(schedule["id"], event_time)

        if succeeded:
            coordinate_result = complete_schedule_success(
                schedule["id"],
                event_time,
            )

            if coordinate_result is None:
                self._clear_active_schedule()
                return

            if coordinate_result["blister_exhausted"]:
                self._run_callback(self.on_blister_exhausted)
        else:
            update_status(schedule["id"], "MISSED")

        self._clear_active_schedule()

    def _process_schedules(self):
        current_monotonic = time.monotonic()

        if current_monotonic - self._last_schedule_check_at < 0.5:
            return

        self._last_schedule_check_at = current_monotonic

        if not self._system_time_is_ready():
            return

        now = datetime.now()
        now_text = now.strftime("%Y-%m-%d %H:%M:%S")

        expire_unstarted_schedules(now_text)
        schedule = self._get_active_schedule()

        if schedule is None:
            schedule = get_active_schedule()

            if schedule is None:
                schedule = get_due_schedule(
                    now.strftime("%Y-%m-%d %H:%M")
                )

                if schedule is not None:
                    x_coordinate, y_coordinate = get_current_coordinate()
                    schedule = assign_coordinate(
                        schedule["id"],
                        x_coordinate,
                        y_coordinate,
                    )

            if schedule is not None:
                with self._state_lock:
                    self._active_schedule_id = schedule["id"]
                    self._last_transmit_at = None

        if schedule is None:
            return

        if schedule["status"] == "ALLOWED":
            deadline_start_text = (
                schedule["ack_received_at"]
                or schedule["command_sent_at"]
            )

            if deadline_start_text is None:
                return

            deadline_start = datetime.strptime(
                deadline_start_text,
                "%Y-%m-%d %H:%M:%S",
            )
            result_deadline = deadline_start + timedelta(
                seconds=schedule["allowed_seconds"]
            )

            if now >= result_deadline:
                update_status(schedule["id"], "MISSED")
                print(
                    f"[UART] 일정 {schedule['id']} "
                    "허용시간 종료: MISSED"
                )
                self._clear_active_schedule()

            return

        if schedule["status"] != "WAITING":
            return

        if self._serial is None:
            return

        if (
            self._last_transmit_at is not None
            and current_monotonic - self._last_transmit_at
            < self.ack_retry_seconds
        ):
            return

        command = build_schedule_command(
            schedule["x_coordinate"],
            schedule["y_coordinate"],
            schedule["allowed_seconds"],
        )
        first_transmission = schedule["command_sent_at"] is None

        if self._write(command):
            record_command_sent(schedule["id"], now_text)
            self._last_transmit_at = current_monotonic

            if first_transmission:
                self._run_callback(self.on_schedule_started)

    def _write(self, payload):
        if self._serial is None:
            return False

        try:
            self._serial.write(payload)
            self._serial.flush()
            print(
                f"[UART] Pi → AT: "
                f"{payload.decode('ascii').strip()!r}"
            )
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

        if schedule is None or schedule["status"] in {
            "DISPENSED",
            "MISSED",
        }:
            self._clear_active_schedule()
            return None

        return schedule

    def _clear_active_schedule(self):
        with self._state_lock:
            self._active_schedule_id = None
            self._last_transmit_at = None

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
