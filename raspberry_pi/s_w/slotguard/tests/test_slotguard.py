import sqlite3
import tempfile
import time
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest.mock import patch

import app
import database
import system_time_service
from uart_service import (
    UartService,
    build_dispense_command,
    build_move_command,
    build_result_ack,
    build_timeout_command,
    parse_protocol_message,
    parse_result_message,
)


class FakeSerial:
    def __init__(self):
        self.writes = []
        self.incoming = bytearray()

    def write(self, payload):
        self.writes.append(payload)

    @property
    def in_waiting(self):
        return len(self.incoming)

    def read(self, size):
        payload = bytes(self.incoming[:size])
        del self.incoming[:size]
        return payload

    def feed(self, payload):
        self.incoming.extend(payload)

    def flush(self):
        pass

    def close(self):
        pass


class SlotguardTestCase(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.original_db_path = database.DB_PATH
        database.DB_PATH = Path(self.temp_dir.name) / "slotguard.db"
        database.init_db()

    def tearDown(self):
        database.DB_PATH = self.original_db_path
        self.temp_dir.cleanup()

    @staticmethod
    def now_minute():
        return datetime.now().strftime("%Y-%m-%d %H:%M")

    @staticmethod
    def now_seconds():
        return datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    def create_schedule(self, scheduled_at=None, allowed_seconds=3600):
        return database.create_schedule(
            scheduled_at or self.now_minute(),
            "혈압약",
            allowed_seconds,
        )

    def make_service(self, **callbacks):
        service = UartService(**callbacks)
        service._serial = FakeSerial()
        service._connection_state = "CONNECTED"
        return service

    def set_coordinate(self, x_coordinate, y_coordinate):
        conn = database.connect_db()
        conn.execute(
            """
            UPDATE device_state
            SET current_x = ?, current_y = ?, blister_exhausted = 0
            WHERE singleton = 1
            """,
            (x_coordinate, y_coordinate),
        )
        conn.commit()
        conn.close()

    def start_move(self, service=None):
        schedule_id = self.create_schedule()
        service = service or self.make_service()
        service._process_schedules()
        return service, schedule_id, database.get_schedule(schedule_id)

    def move_to_ready(self, service=None):
        service, schedule_id, schedule = self.start_move(service)
        request_id = schedule["move_request_id"]
        service._handle_message(f"ACK|{request_id}|MOVE")
        service._handle_message(f"WAIT|{request_id}")
        return service, schedule_id, database.get_schedule(schedule_id)

    def move_to_dispensing(self, service=None):
        service, schedule_id, schedule = self.move_to_ready(service)
        schedule = service.request_dispense(schedule_id)
        return service, schedule_id, schedule

    def finish_home_return(self, service, schedule_id):
        returning = database.get_schedule(schedule_id)
        request_id = returning["home_request_id"]
        self.assertIsNotNone(request_id)
        self.assertEqual(
            service._serial.writes[-1],
            f"MOVE|{request_id}|0|0|000030\n".encode("ascii"),
        )
        service._handle_message(f"ACK|{request_id}|MOVE")
        service._handle_message(f"WAIT|{request_id}")
        self.assertEqual(
            service._serial.writes[-1],
            f"TIMEOUT|{request_id}\n".encode("ascii"),
        )
        service._handle_message(f"ACK|{request_id}|TIMEOUT")
        return database.get_schedule(schedule_id)

    def test_protocol_v2_messages(self):
        self.assertEqual(
            build_move_command("00000001", 0, 0, 3600),
            b"MOVE|00000001|0|0|003600\n",
        )
        self.assertEqual(
            build_dispense_command("00000002", 1, 4),
            b"DISPENSE|00000002|1|4\n",
        )
        self.assertEqual(
            build_result_ack("00000002"),
            b"ACK|00000002|RESULT\n",
        )
        self.assertEqual(
            build_timeout_command("00000001"),
            b"TIMEOUT|00000001\n",
        )
        self.assertEqual(
            parse_protocol_message("ACK|00000001|TIMEOUT"),
            {
                "type": "ACK",
                "request_id": "00000001",
                "action": "TIMEOUT",
            },
        )
        self.assertEqual(parse_result_message("141"), (1, 4, 1))
        self.assertEqual(parse_result_message("142"), (1, 4, 2))
        self.assertEqual(
            parse_protocol_message("RESULT|00000002|140"),
            {
                "type": "RESULT",
                "request_id": "00000002",
                "result": (1, 4, 0),
            },
        )
        self.assertEqual(
            parse_protocol_message("ERROR|INVALID_FORMAT"),
            {"type": "INVALID_FORMAT"},
        )
        self.assertIsNone(
            parse_protocol_message("ERROR|00000001|INVALID_FORMAT")
        )
        self.assertIsNone(parse_protocol_message("WAIT|invalid"))
        self.assertIsNone(parse_result_message("143"))

    def test_schedule_requires_medicine_and_rejects_duplicate_time(self):
        schedule_id = self.create_schedule()
        schedule = database.get_schedule(schedule_id)
        self.assertEqual(schedule["medicine_name"], "혈압약")
        self.assertEqual(schedule["status"], "SCHEDULED")

        with self.assertRaises(database.DuplicateScheduleError):
            self.create_schedule()
        with self.assertRaises(ValueError):
            database.create_schedule(
                "2099-01-02 09:00",
                "가" * 31,
                3600,
            )

    def test_move_wait_dispense_result_flow_uses_two_request_ids(self):
        service, schedule_id, schedule = self.move_to_dispensing()
        self.assertEqual(schedule["status"], "DISPENSING")
        self.assertEqual(schedule["move_request_id"], "00000001")
        self.assertEqual(schedule["dispense_request_id"], "00000002")
        self.assertEqual(
            service._serial.writes,
            [
                b"MOVE|00000001|0|0|003600\n",
                b"DISPENSE|00000002|0|0\n",
            ],
        )

        service._handle_message("ACK|00000002|DISPENSE")
        service._handle_message("RESULT|00000002|001")

        completed = database.get_schedule(schedule_id)
        self.assertEqual(completed["status"], "DISPENSED")
        self.assertEqual(database.get_current_coordinate(), (0, 1))
        self.assertEqual(
            service._serial.writes[-1],
            b"ACK|00000002|RESULT\n",
        )

    def test_move_retry_reuses_request_id_until_ack(self):
        service, schedule_id, schedule = self.start_move()
        first_payload = service._serial.writes[0]

        service._last_schedule_check_at = 0
        service._last_move_transmit_at = (
            time.monotonic() - service.ack_retry_seconds
        )
        service._process_schedules()
        self.assertEqual(service._serial.writes, [first_payload, first_payload])

        service._handle_message(
            f"ACK|{schedule['move_request_id']}|MOVE"
        )
        service._last_schedule_check_at = 0
        service._last_move_transmit_at = (
            time.monotonic() - service.ack_retry_seconds
        )
        service._process_schedules()
        self.assertEqual(service._serial.writes, [first_payload, first_payload])
        self.assertIsNotNone(database.get_schedule(schedule_id)["move_ack_at"])

    def test_dispense_retry_reuses_request_id_until_ack(self):
        service, schedule_id, schedule = self.move_to_dispensing()
        dispense_payload = service._serial.writes[-1]
        service._last_schedule_check_at = 0
        service._last_dispense_transmit_at = (
            time.monotonic() - service.ack_retry_seconds
        )
        service._process_schedules()
        self.assertEqual(service._serial.writes[-1], dispense_payload)
        self.assertEqual(service._serial.writes.count(dispense_payload), 2)

        service._handle_message(
            f"ACK|{schedule['dispense_request_id']}|DISPENSE"
        )
        service._last_schedule_check_at = 0
        service._last_dispense_transmit_at = (
            time.monotonic() - service.ack_retry_seconds
        )
        service._process_schedules()
        self.assertEqual(service._serial.writes.count(dispense_payload), 2)

    def test_invalid_format_retries_same_move_without_failing_schedule(self):
        service, schedule_id, schedule = self.start_move()
        payload = service._serial.writes[-1]
        service._handle_message("ERROR|INVALID_FORMAT")

        self.assertEqual(service._serial.writes, [payload, payload])
        current = database.get_schedule(schedule_id)
        self.assertEqual(current["status"], "MOVING")
        self.assertIsNone(current["error_code"])

    def test_invalid_format_without_previous_transmission_only_logs_error(self):
        service = self.make_service()

        service._handle_message("ERROR|INVALID_FORMAT")

        self.assertEqual(service._serial.writes, [])
        self.assertIn(
            "재전송할 마지막 UART 프레임이 없습니다",
            service.get_status()["last_error"],
        )

    def test_non_format_device_error_still_ends_as_comm_error(self):
        service, schedule_id, schedule = self.start_move()
        service._handle_message(
            f"ERROR|{schedule['move_request_id']}|STEPPER_ERROR"
        )

        current = database.get_schedule(schedule_id)
        self.assertEqual(current["status"], "COMM_ERROR")
        self.assertEqual(current["error_code"], "AT_STEPPER_ERROR")
        self.assertIsNone(service.get_status()["active_schedule_id"])

    def test_invalid_format_retries_same_dispense_without_failing_schedule(self):
        service, schedule_id, schedule = self.move_to_dispensing()
        payload = service._serial.writes[-1]
        service._handle_message("ERROR|INVALID_FORMAT")

        self.assertEqual(service._serial.writes.count(payload), 2)
        current = database.get_schedule(schedule_id)
        self.assertEqual(current["status"], "DISPENSING")
        self.assertIsNone(current["error_code"])

    def test_invalid_format_retries_result_ack_after_completion(self):
        service, schedule_id, schedule = self.move_to_dispensing()
        request_id = schedule["dispense_request_id"]
        service._handle_message(f"RESULT|{request_id}|001")
        ack_payload = build_result_ack(request_id)
        service._handle_message("ERROR|INVALID_FORMAT")

        self.assertEqual(service._serial.writes.count(ack_payload), 2)
        self.assertEqual(
            database.get_schedule(schedule_id)["status"],
            "DISPENSED",
        )

    def test_wait_is_implicit_move_ack_and_enables_button(self):
        service, schedule_id, schedule = self.start_move()
        service._handle_message(f"WAIT|{schedule['move_request_id']}")
        ready = database.get_schedule(schedule_id)
        self.assertEqual(ready["status"], "READY_TO_DISPENSE")
        self.assertIsNotNone(ready["move_ack_at"])
        self.assertIsNotNone(ready["ready_at"])

    def test_result_failure_can_be_manually_completed_and_advances(self):
        service, schedule_id, schedule = self.move_to_dispensing()
        service._handle_message(
            f"RESULT|{schedule['dispense_request_id']}|000"
        )
        failed = database.get_schedule(schedule_id)
        self.assertEqual(failed["status"], "FAILED")
        self.assertEqual(failed["error_code"], "NO_DROP_DETECTED")
        self.assertEqual(database.get_current_coordinate(), (0, 0))

        result = database.complete_manual(schedule_id, self.now_seconds())
        self.assertEqual(
            database.get_schedule(schedule_id)["status"],
            "MANUALLY_COMPLETED",
        )
        self.assertEqual(database.get_current_coordinate(), (0, 1))
        self.assertFalse(result["blister_exhausted"])

    def test_failed_result_can_be_acknowledged_without_advancing(self):
        service, schedule_id, schedule = self.move_to_dispensing()
        service._handle_message(
            f"RESULT|{schedule['dispense_request_id']}|002"
        )
        retry = database.get_schedule(schedule_id)
        service._handle_message(f"WAIT|{retry['move_request_id']}")
        retry = service.request_dispense(schedule_id)
        service._handle_message(
            f"RESULT|{retry['dispense_request_id']}|010"
        )
        failed = database.get_schedule(schedule_id)
        self.assertEqual(failed["error_code"], "NO_DROP_DETECTED")
        self.assertIsNotNone(database.get_unacknowledged_result())
        database.acknowledge_result(schedule_id, self.now_seconds())
        self.assertIsNone(database.get_unacknowledged_result())
        self.assertEqual(database.get_current_coordinate(), (0, 1))

    def test_empty_slots_repeat_with_new_ids_until_success(self):
        service, schedule_id, first_attempt = self.move_to_dispensing()
        first_result = (
            f"RESULT|{first_attempt['dispense_request_id']}|002"
        )
        service._handle_message(first_result)

        first_retry = database.get_schedule(schedule_id)
        self.assertEqual(first_retry["status"], "MOVING")
        self.assertEqual(first_retry["error_code"], "EMPTY_BLISTER_SLOT")
        self.assertEqual(
            (first_retry["x_coordinate"], first_retry["y_coordinate"]),
            (0, 1),
        )
        self.assertEqual(first_retry["move_request_id"], "00000003")
        self.assertIsNone(first_retry["dispense_request_id"])
        self.assertGreater(first_retry["move_allowed_seconds"], 0)
        self.assertLessEqual(
            first_retry["move_allowed_seconds"],
            first_retry["allowed_seconds"],
        )
        self.assertEqual(
            service._serial.writes[-2:],
            [
                b"ACK|00000002|RESULT\n",
                (
                    "MOVE|00000003|0|1|"
                    f"{first_retry['move_allowed_seconds']:06d}\n"
                ).encode("ascii"),
            ],
        )

        retry_move_payload = service._serial.writes[-1]
        service._last_schedule_check_at = 0
        service._last_move_transmit_at = (
            time.monotonic() - service.ack_retry_seconds
        )
        service._process_schedules()
        self.assertEqual(service._serial.writes[-1], retry_move_payload)
        self.assertEqual(
            service._serial.writes.count(retry_move_payload),
            2,
        )

        service._handle_message(first_result)
        self.assertEqual(database.get_current_coordinate(), (0, 1))
        self.assertEqual(
            service._serial.writes.count(b"ACK|00000002|RESULT\n"),
            2,
        )

        service._handle_message("WAIT|00000003")
        second_attempt = service.request_dispense(schedule_id)
        self.assertEqual(second_attempt["dispense_request_id"], "00000004")
        service._handle_message("RESULT|00000004|012")

        second_retry = database.get_schedule(schedule_id)
        self.assertEqual(second_retry["move_request_id"], "00000005")
        self.assertEqual(database.get_current_coordinate(), (0, 2))
        service._handle_message("WAIT|00000005")
        final_attempt = service.request_dispense(schedule_id)
        self.assertEqual(final_attempt["dispense_request_id"], "00000006")
        service._handle_message("RESULT|00000006|021")

        completed = database.get_schedule(schedule_id)
        self.assertEqual(completed["status"], "DISPENSED")
        self.assertIsNone(completed["error_code"])
        self.assertEqual(database.get_current_coordinate(), (0, 3))
        empty_events = [
            event
            for event in database.get_event_log()
            if event["event_type"] == "EMPTY_BLISTER_SLOT"
        ]
        self.assertEqual(len(empty_events), 2)
        self.assertEqual(
            {event["detail"] for event in empty_events},
            {"00", "01"},
        )

    def test_duplicate_result_is_acknowledged_without_double_advance(self):
        service, schedule_id, schedule = self.move_to_dispensing()
        result_message = f"RESULT|{schedule['dispense_request_id']}|001"
        service._handle_message(result_message)
        service._handle_message(result_message)
        self.assertEqual(database.get_current_coordinate(), (0, 1))
        self.assertEqual(
            service._serial.writes.count(
                build_result_ack(schedule["dispense_request_id"])
            ),
            2,
        )

    def test_mismatched_result_coordinate_is_ignored(self):
        service, schedule_id, schedule = self.move_to_dispensing()
        service._handle_message(
            f"RESULT|{schedule['dispense_request_id']}|011"
        )
        self.assertEqual(
            database.get_schedule(schedule_id)["status"],
            "DISPENSING",
        )
        self.assertIn("결과 좌표 불일치", service.get_status()["last_error"])

    def test_window_starts_at_scheduled_time_and_old_schedule_is_missed(self):
        old_time = (datetime.now() - timedelta(minutes=2)).strftime(
            "%Y-%m-%d %H:%M"
        )
        schedule_id = self.create_schedule(old_time, 60)
        service = self.make_service()
        service._process_schedules()
        schedule = database.get_schedule(schedule_id)
        self.assertEqual(schedule["status"], "MISSED")
        self.assertEqual(schedule["error_code"], "DOSE_WINDOW_EXPIRED")
        self.assertIsNone(schedule["timeout_requested_at"])
        self.assertEqual(service._serial.writes, [])

    def test_ready_timeout_is_persisted_retried_and_acknowledged(self):
        service, schedule_id, schedule = self.move_to_ready()
        request_id = schedule["move_request_id"]
        timeout_payload = build_timeout_command(request_id)
        conn = database.connect_db()
        conn.execute(
            "UPDATE schedules SET scheduled_at = ? WHERE id = ?",
            (
                (datetime.now() - timedelta(hours=2)).strftime(
                    "%Y-%m-%d %H:%M"
                ),
                schedule_id,
            ),
        )
        conn.commit()
        conn.close()

        service._last_schedule_check_at = 0
        service._process_schedules()
        expired = database.get_schedule(schedule_id)
        self.assertEqual(expired["status"], "MISSED")
        self.assertEqual(expired["error_code"], "DOSE_BUTTON_TIMEOUT")
        self.assertIsNotNone(expired["timeout_requested_at"])
        self.assertIsNotNone(expired["timeout_sent_at"])
        self.assertIsNone(expired["timeout_ack_at"])
        self.assertEqual(service._serial.writes[-1], timeout_payload)

        service._last_schedule_check_at = 0
        service._last_timeout_transmit_at = (
            time.monotonic() - service.ack_retry_seconds
        )
        service._process_schedules()
        self.assertEqual(service._serial.writes.count(timeout_payload), 2)

        database.acknowledge_result(schedule_id, self.now_seconds())
        next_schedule_id = self.create_schedule()
        restarted_service = self.make_service()
        restarted_service._process_schedules()
        self.assertEqual(restarted_service._serial.writes, [timeout_payload])
        self.assertEqual(
            database.get_schedule(next_schedule_id)["status"],
            "SCHEDULED",
        )

        restarted_service._handle_message(f"ACK|{request_id}|TIMEOUT")
        acknowledged = database.get_schedule(schedule_id)
        self.assertIsNotNone(acknowledged["timeout_ack_at"])
        self.assertIsNone(database.get_pending_timeout())

        restarted_service._last_schedule_check_at = 0
        restarted_service._process_schedules()
        self.assertEqual(
            database.get_schedule(next_schedule_id)["status"],
            "MOVING",
        )

        restarted_service._handle_message(f"ACK|{request_id}|TIMEOUT")
        self.assertNotIn(
            "불일치",
            restarted_service.get_status()["last_error"] or "",
        )

    def test_expired_dispense_request_queues_timeout(self):
        service, schedule_id, schedule = self.move_to_ready()
        conn = database.connect_db()
        conn.execute(
            "UPDATE schedules SET scheduled_at = ? WHERE id = ?",
            (
                (datetime.now() - timedelta(hours=2)).strftime(
                    "%Y-%m-%d %H:%M"
                ),
                schedule_id,
            ),
        )
        conn.commit()
        conn.close()

        with self.assertRaisesRegex(ValueError, "허용시간"):
            service.request_dispense(schedule_id)
        expired = database.get_schedule(schedule_id)
        self.assertEqual(expired["status"], "MISSED")
        self.assertEqual(
            service._serial.writes[-1],
            build_timeout_command(schedule["move_request_id"]),
        )

    def test_invalid_format_retries_same_timeout_without_changing_missed(self):
        service, schedule_id, schedule = self.move_to_ready()
        request_id = schedule["move_request_id"]
        expired = database.mark_missed_and_queue_timeout(
            schedule_id,
            self.now_seconds(),
        )
        service._clear_active_schedule()
        service._transmit_timeout(expired, force=True)
        timeout_payload = build_timeout_command(request_id)

        service._handle_message("ERROR|INVALID_FORMAT")

        self.assertEqual(service._serial.writes.count(timeout_payload), 2)
        current = database.get_schedule(schedule_id)
        self.assertEqual(current["status"], "MISSED")
        self.assertIsNone(current["timeout_ack_at"])

    def test_move_ack_timeout_is_comm_error(self):
        service, schedule_id, schedule = self.start_move()
        conn = database.connect_db()
        conn.execute(
            "UPDATE schedules SET scheduled_at = ? WHERE id = ?",
            (
                (datetime.now() - timedelta(hours=2)).strftime(
                    "%Y-%m-%d %H:%M"
                ),
                schedule_id,
            ),
        )
        conn.commit()
        conn.close()
        service._last_schedule_check_at = 0
        service._process_schedules()
        expired = database.get_schedule(schedule_id)
        self.assertEqual(expired["status"], "COMM_ERROR")
        self.assertEqual(expired["error_code"], "MOVE_ACK_TIMEOUT")
        self.assertIsNone(expired["timeout_requested_at"])
        self.assertNotIn(
            build_timeout_command(schedule["move_request_id"]),
            service._serial.writes,
        )

    def test_scheduler_is_blocked_until_system_time_is_set(self):
        schedule_id = self.create_schedule()
        service = self.make_service(is_time_ready=lambda: False)
        service._process_schedules()
        self.assertEqual(service._serial.writes, [])
        self.assertEqual(
            database.get_schedule(schedule_id)["status"],
            "SCHEDULED",
        )

    def test_uart_lines_can_arrive_in_partial_chunks(self):
        service, schedule_id, schedule = self.start_move()
        request_id = schedule["move_request_id"]
        service._serial.feed(f"ACK|{request_id}|MO".encode("ascii"))
        service._read_available_lines()
        self.assertIsNone(database.get_schedule(schedule_id)["move_ack_at"])
        service._serial.feed(b"VE\n")
        service._read_available_lines()
        self.assertIsNotNone(database.get_schedule(schedule_id)["move_ack_at"])

    def test_serpentine_coordinate_path_reduces_row_transition(self):
        expected_coordinates = [
            (0, 1),
            (0, 2),
            (0, 3),
            (0, 4),
            (1, 4),
            (1, 3),
            (1, 2),
            (1, 1),
            (1, 0),
            (1, 0),
        ]
        for expected in expected_coordinates:
            result = database.advance_coordinate(self.now_seconds())
            self.assertEqual(result["current"], expected)

        self.assertTrue(result["blister_exhausted"])
        self.assertEqual(
            database.get_used_coordinates(),
            list(database.SLOT_COORDINATE_PATH),
        )

    def test_last_slot_stays_exhausted_until_gui_reset(self):
        conn = database.connect_db()
        conn.execute(
            """
            UPDATE device_state
            SET current_x = 1, current_y = 0, blister_exhausted = 0
            WHERE singleton = 1
            """
        )
        conn.commit()
        conn.close()
        future_id = database.create_schedule(
            "2099-01-01 09:00",
            "미래 약",
            3600,
        )
        active_id = database.create_schedule(
            self.now_minute(),
            "현재 약",
            3600,
        )
        service = self.make_service()
        service._process_schedules()
        active = database.get_schedule(active_id)
        service._handle_message(f"WAIT|{active['move_request_id']}")
        active = service.request_dispense(active_id)
        service._handle_message(
            f"RESULT|{active['dispense_request_id']}|101"
        )
        self.assertTrue(database.get_device_state()["blister_exhausted"])
        self.assertEqual(len(database.get_used_coordinates()), 10)
        self.assertEqual(database.get_current_coordinate(), (1, 0))
        self.assertEqual(app.build_display_status()["screen"], "RETURNING_HOME")
        with self.assertRaisesRegex(database.ActiveDoseError, "복귀"):
            database.reset_blister(self.now_seconds())

        self.finish_home_return(service, active_id)
        self.assertEqual(database.get_current_coordinate(), (0, 0))
        self.assertEqual(app.build_display_status()["screen"], "BLISTER_EMPTY")

        database.reset_blister(self.now_seconds())
        self.assertFalse(database.get_device_state()["blister_exhausted"])
        self.assertEqual(database.get_current_coordinate(), (0, 0))
        self.assertIsNotNone(database.get_schedule(future_id))

    def test_last_empty_slot_manual_taken_keeps_new_blister_at_00(self):
        self.set_coordinate(1, 0)
        service, schedule_id, attempt = self.move_to_dispensing()
        service._handle_message(
            f"RESULT|{attempt['dispense_request_id']}|102"
        )

        empty = database.get_schedule(schedule_id)
        self.assertEqual(empty["status"], "FAILED")
        self.assertEqual(empty["error_code"], "EMPTY_BLISTER_SLOT")
        self.assertTrue(database.get_device_state()["blister_exhausted"])
        self.assertFalse(
            database.acknowledge_result(schedule_id, self.now_seconds())
        )
        self.assertEqual(app.build_display_status()["screen"], "RETURNING_HOME")

        self.finish_home_return(service, schedule_id)
        self.assertEqual(app.build_display_status()["screen"], "BLISTER_EMPTY")

        database.reset_blister(self.now_seconds())
        reset = database.get_schedule(schedule_id)
        self.assertEqual(
            (reset["x_coordinate"], reset["y_coordinate"]),
            (0, 0),
        )
        self.assertEqual(
            app.build_display_status()["screen"],
            "EMPTY_BLISTER_CONFIRM",
        )

        response = app.app.test_client().post(
            "/api/display/empty-blister-choice",
            json={"schedule_id": schedule_id, "choice": "manual_taken"},
            environ_base={"REMOTE_ADDR": "127.0.0.1"},
        )
        self.assertEqual(response.status_code, 200)
        completed = database.get_schedule(schedule_id)
        self.assertEqual(completed["status"], "MANUALLY_COMPLETED")
        self.assertIsNone(completed["error_code"])
        self.assertEqual(database.get_current_coordinate(), (0, 0))

    def test_last_empty_slot_manual_not_taken_restarts_dispense_at_00(self):
        self.set_coordinate(1, 0)
        service, schedule_id, attempt = self.move_to_dispensing()
        service._handle_message(
            f"RESULT|{attempt['dispense_request_id']}|102"
        )
        self.finish_home_return(service, schedule_id)
        database.reset_blister(self.now_seconds())

        with patch.object(app, "uart_service", service):
            response = app.app.test_client().post(
                "/api/display/empty-blister-choice",
                json={
                    "schedule_id": schedule_id,
                    "choice": "manual_not_taken",
                },
                environ_base={"REMOTE_ADDR": "127.0.0.1"},
            )
        self.assertEqual(response.status_code, 200)
        resumed = database.get_schedule(schedule_id)
        self.assertEqual(resumed["status"], "MOVING")
        self.assertEqual(resumed["move_request_id"], "00000004")
        self.assertEqual(
            (resumed["x_coordinate"], resumed["y_coordinate"]),
            (0, 0),
        )
        self.assertEqual(
            service._serial.writes[-1],
            (
                "MOVE|00000004|0|0|"
                f"{resumed['move_allowed_seconds']:06d}\n"
            ).encode("ascii"),
        )

        service._handle_message("WAIT|00000004")
        retry = service.request_dispense(schedule_id)
        self.assertEqual(retry["dispense_request_id"], "00000005")
        service._handle_message("RESULT|00000005|001")
        completed = database.get_schedule(schedule_id)
        self.assertEqual(completed["status"], "DISPENSED")
        self.assertIsNone(completed["error_code"])
        self.assertEqual(database.get_current_coordinate(), (0, 1))

    def test_last_empty_slot_manual_not_taken_expires_as_missed(self):
        self.set_coordinate(1, 0)
        service, schedule_id, attempt = self.move_to_dispensing()
        service._handle_message(
            f"RESULT|{attempt['dispense_request_id']}|102"
        )
        self.finish_home_return(service, schedule_id)
        database.reset_blister(self.now_seconds())
        writes_before_resume = list(service._serial.writes)

        conn = database.connect_db()
        conn.execute(
            "UPDATE schedules SET scheduled_at = ? WHERE id = ?",
            (
                (datetime.now() - timedelta(hours=2)).strftime(
                    "%Y-%m-%d %H:%M"
                ),
                schedule_id,
            ),
        )
        conn.commit()
        conn.close()

        expired = service.resume_empty_blister_dose(schedule_id)
        self.assertEqual(expired["status"], "MISSED")
        self.assertEqual(expired["error_code"], "DOSE_WINDOW_EXPIRED")
        self.assertEqual(service._serial.writes, writes_before_resume)
        self.assertEqual(database.get_current_coordinate(), (0, 0))

    def test_empty_blister_choice_uses_two_large_side_by_side_buttons(self):
        base_dir = Path(__file__).resolve().parents[1]
        javascript = (base_dir / "static" / "display.js").read_text(
            encoding="utf-8"
        )
        stylesheet = (base_dir / "static" / "display.css").read_text(
            encoding="utf-8"
        )
        self.assertIn("수동복약", javascript)
        self.assertIn("수동미복약", javascript)
        self.assertIn("lcd-empty-choice-actions", javascript)
        self.assertIn(
            "grid-template-columns: repeat(2, minmax(0, 1fr));",
            stylesheet,
        )
        self.assertIn("font-size: clamp(30px, 7vw, 42px);", stylesheet)

    def test_blister_reset_is_blocked_during_active_dose(self):
        service, _, _ = self.start_move()
        with self.assertRaises(database.ActiveDoseError):
            database.reset_blister(self.now_seconds())
        self.assertIsNotNone(service.get_status()["active_schedule_id"])

    def test_voice_settings_persist(self):
        database.update_device_settings(10, 10, self.now_seconds())
        settings = database.get_device_settings()
        self.assertEqual(settings["voice_repeat"], 10)
        self.assertEqual(settings["volume_step"], 10)

        database.update_device_settings(1, 0, self.now_seconds())
        settings = database.get_device_settings()
        self.assertEqual(settings["volume_step"], 0)

        database.update_device_settings(0, 5, self.now_seconds())
        settings = database.get_device_settings()
        self.assertEqual(settings["voice_repeat"], 0)

        with self.assertRaises(ValueError):
            database.update_device_settings(1, 11, self.now_seconds())
        with self.assertRaises(ValueError):
            database.update_device_settings(-1, 5, self.now_seconds())

    def test_display_endpoint_and_local_write_protection(self):
        client = app.app.test_client()
        response = client.get("/display")
        self.assertEqual(response.status_code, 200)
        self.assertIn("약SLOT-GUARD".encode("utf-8"), response.data)

        response = client.post(
            "/api/display/settings",
            json={"voice_repeat": 3, "volume_step": 3},
            environ_base={"REMOTE_ADDR": "192.168.0.20"},
        )
        self.assertEqual(response.status_code, 403)

        response = client.post(
            "/api/display/settings",
            json={"voice_repeat": 3, "volume_step": 3},
            environ_base={"REMOTE_ADDR": "127.0.0.1"},
        )
        self.assertEqual(response.status_code, 200)
        self.assertEqual(database.get_device_settings()["voice_repeat"], 3)
        self.assertEqual(database.get_device_settings()["volume_step"], 3)

    def test_legacy_volume_labels_migrate_to_numeric_steps(self):
        database.DB_PATH.unlink()
        conn = sqlite3.connect(database.DB_PATH)
        conn.execute(
            """
            CREATE TABLE device_settings (
                singleton INTEGER PRIMARY KEY,
                voice_repeat INTEGER NOT NULL,
                volume_level TEXT NOT NULL,
                updated_at TEXT
            )
            """
        )
        conn.execute(
            """
            INSERT INTO device_settings (
                singleton, voice_repeat, volume_level
            ) VALUES (1, 4, 'high')
            """
        )
        conn.commit()
        conn.close()

        database.init_db()
        settings = database.get_device_settings()
        self.assertEqual(settings["voice_repeat"], 4)
        self.assertEqual(settings["volume_step"], 8)
        database.update_device_settings(0, 8, self.now_seconds())
        self.assertEqual(database.get_device_settings()["voice_repeat"], 0)

    def test_display_status_contains_next_medicine_and_slots(self):
        self.create_schedule("2099-01-01 09:00")
        with patch.object(app, "get_local_ip_address", return_value="192.168.0.2"):
            status = app.build_display_status()
        self.assertEqual(status["next_schedule"]["medicine_name"], "혈압약")
        self.assertEqual(status["target_coordinate"], {"x": 0, "y": 0})
        self.assertEqual(status["device"]["network"], "CONNECTED")
        self.assertNotIn("management_url", status["device"])
        self.assertNotIn("local_ip", status["device"])

        database.update_device_settings(0, 5, self.now_seconds())
        status = app.build_display_status()
        self.assertEqual(status["device"]["audio"], "DISABLED")

    def test_display_status_returns_latest_three_records_across_dates(self):
        record_ids = []
        for day in range(15, 19):
            schedule_id = self.create_schedule(f"2024-08-{day:02d} 11:13")
            record_ids.append(schedule_id)
            conn = database.connect_db()
            conn.execute(
                "UPDATE schedules SET status = 'DISPENSED', completed_at = ? "
                "WHERE id = ?",
                (f"2024-08-{day:02d} 11:13:00", schedule_id),
            )
            conn.commit()
            conn.close()

        with patch.object(app, "get_local_ip_address", return_value="192.168.0.2"):
            status = app.build_display_status()

        self.assertEqual(
            [record["id"] for record in status["recent_records"]],
            list(reversed(record_ids[-3:])),
        )
        self.assertEqual(
            [record["completed_at"] for record in status["recent_records"]],
            [
                "2024-08-18 11:13:00",
                "2024-08-17 11:13:00",
                "2024-08-16 11:13:00",
            ],
        )

    def test_volume_test_uses_saved_step_and_rejects_mute(self):
        client = app.app.test_client()
        database.update_device_settings(2, 7, self.now_seconds())
        with patch.object(app.voice_alert_manager, "test_once", return_value=True) as test_once:
            response = client.post(
                "/api/display/test-volume",
                json={},
                environ_base={"REMOTE_ADDR": "127.0.0.1"},
            )
        self.assertEqual(response.status_code, 200)
        test_once.assert_called_once_with(7)

        database.update_device_settings(2, 0, self.now_seconds())
        with patch.object(app.voice_alert_manager, "test_once", return_value=False):
            response = client.post(
                "/api/display/test-volume",
                json={},
                environ_base={"REMOTE_ADDR": "127.0.0.1"},
            )
        self.assertEqual(response.status_code, 409)
        self.assertEqual(response.get_json()["error"], "VOLUME_MUTED")

    def test_smartphone_time_is_valid_only_for_current_boot(self):
        boot_id_path = Path(self.temp_dir.name) / "boot_id"
        boot_id_path.write_text("boot-one\n", encoding="ascii")
        timestamp_ms = int(
            datetime(
                2026,
                8,
                13,
                12,
                0,
                tzinfo=timezone.utc,
            ).timestamp()
            * 1000
        )

        with (
            patch.object(system_time_service, "BOOT_ID_PATH", boot_id_path),
            patch.object(system_time_service, "_run_time_helper") as helper,
        ):
            result = system_time_service.set_system_time_from_smartphone(
                timestamp_ms,
                "Asia/Seoul",
            )
            self.assertTrue(result["configured"])
            self.assertTrue(system_time_service.is_system_time_configured())
            helper.assert_called_once()

            boot_id_path.write_text("boot-two\n", encoding="ascii")
            self.assertFalse(system_time_service.is_system_time_configured())

    def test_legacy_database_migrates_without_losing_schedule(self):
        database.DB_PATH.unlink()
        conn = sqlite3.connect(database.DB_PATH)
        conn.execute(
            """
            CREATE TABLE schedules (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                scheduled_at TEXT NOT NULL,
                allowed_seconds INTEGER NOT NULL,
                status TEXT NOT NULL,
                x_coordinate INTEGER,
                y_coordinate INTEGER,
                dispensed_at TEXT,
                synced INTEGER NOT NULL DEFAULT 0,
                command_sent_at TEXT,
                ack_received_at TEXT
            )
            """
        )
        conn.execute(
            """
            INSERT INTO schedules (
                scheduled_at, allowed_seconds, status
            )
            VALUES ('2099-01-01 09:00', 3600, 'WAITING')
            """
        )
        conn.commit()
        conn.close()

        database.init_db()
        schedule = database.get_schedules()[0]
        self.assertEqual(schedule["medicine_name"], "등록된 약")
        self.assertEqual(schedule["status"], "SCHEDULED")
        self.assertIsNone(schedule["timeout_requested_at"])
        self.assertIsNone(schedule["timeout_sent_at"])
        self.assertIsNone(schedule["timeout_ack_at"])

    def test_v6_blister_keeps_legacy_path_until_reset(self):
        conn = database.connect_db()
        conn.execute(
            "ALTER TABLE device_state RENAME TO device_state_v7"
        )
        conn.execute(
            """
            CREATE TABLE device_state (
                singleton INTEGER PRIMARY KEY,
                current_x INTEGER NOT NULL,
                current_y INTEGER NOT NULL,
                blister_exhausted INTEGER NOT NULL DEFAULT 0,
                updated_at TEXT
            )
            """
        )
        conn.execute(
            "INSERT INTO device_state (singleton, current_x, current_y) "
            "VALUES (1, 1, 0)"
        )
        conn.execute("DROP TABLE device_state_v7")
        conn.execute("PRAGMA user_version = 6")
        conn.commit()
        conn.close()

        database.init_db()
        self.assertEqual(database.get_current_coordinate(), (1, 0))
        self.assertEqual(
            database.get_device_state()["coordinate_path_version"],
            1,
        )
        self.assertEqual(
            database.advance_coordinate(self.now_seconds())["current"],
            (1, 1),
        )
        conn = database.connect_db()
        self.assertEqual(conn.execute("PRAGMA user_version").fetchone()[0], 8)
        conn.close()

        database.reset_blister(self.now_seconds())
        self.assertEqual(
            database.get_device_state()["coordinate_path_version"],
            2,
        )
        self.set_coordinate(0, 4)
        self.assertEqual(
            database.advance_coordinate(self.now_seconds())["current"],
            (1, 4),
        )


if __name__ == "__main__":
    unittest.main()
