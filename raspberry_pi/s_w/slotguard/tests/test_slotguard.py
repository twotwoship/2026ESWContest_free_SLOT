import sqlite3
import tempfile
import time
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import patch

import database
import system_time_service
from uart_service import (
    UartService,
    build_schedule_command,
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

    def create_assigned_schedule(self, coordinate=(0, 0)):
        schedule_id = database.create_schedule(
            "2099-01-01 09:00",
            3600,
        )
        database.assign_coordinate(
            schedule_id,
            coordinate[0],
            coordinate[1],
        )
        return schedule_id

    def make_service(self, on_blister_exhausted=None):
        service = UartService(
            on_blister_exhausted=on_blister_exhausted
        )
        service._serial = FakeSerial()
        return service

    def test_command_and_result_protocol(self):
        self.assertEqual(
            build_schedule_command(0, 0, 3600),
            b"00003600\n",
        )
        self.assertEqual(
            build_schedule_command(1, 4, 60),
            b"14000060\n",
        )
        self.assertEqual(
            build_schedule_command(0, 0, 999999),
            b"00999999\n",
        )
        self.assertEqual(parse_result_message("001"), (0, 0, 1))
        self.assertEqual(parse_result_message("140"), (1, 4, 0))
        self.assertIsNone(parse_result_message("ACK"))
        self.assertIsNone(parse_result_message("205"))

        with self.assertRaises(ValueError):
            build_schedule_command(0, 0, 1000000)

    def test_ack_wait_and_success_advance_coordinate(self):
        schedule_id = self.create_assigned_schedule()
        service = self.make_service()
        service._active_schedule_id = schedule_id

        service._handle_message("ACK")
        service._handle_message("WAIT")
        service._handle_message("001")

        schedule = database.get_schedule(schedule_id)
        self.assertEqual(schedule["status"], "DISPENSED")
        self.assertEqual(schedule["synced"], 1)
        self.assertEqual(database.get_current_coordinate(), (0, 1))
        self.assertEqual(service._serial.writes, [b"ACK\n"])

    def test_failure_keeps_same_coordinate_for_next_schedule(self):
        schedule_id = self.create_assigned_schedule()
        service = self.make_service()
        service._active_schedule_id = schedule_id

        service._handle_message("ACK")
        service._handle_message("000")

        schedule = database.get_schedule(schedule_id)
        self.assertEqual(schedule["status"], "MISSED")
        self.assertEqual(database.get_current_coordinate(), (0, 0))
        self.assertEqual(service._serial.writes, [b"ACK\n"])

    def test_uart_lines_can_arrive_in_partial_chunks(self):
        schedule_id = self.create_assigned_schedule()
        service = self.make_service()
        service._active_schedule_id = schedule_id

        service._serial.feed(b"AC")
        service._read_available_lines()
        self.assertEqual(
            database.get_schedule(schedule_id)["status"],
            "WAITING",
        )

        service._serial.feed(b"K\nWA")
        service._read_available_lines()
        self.assertEqual(
            database.get_schedule(schedule_id)["status"],
            "ALLOWED",
        )

        service._serial.feed(b"IT\n001\n")
        service._read_available_lines()

        self.assertEqual(
            database.get_schedule(schedule_id)["status"],
            "DISPENSED",
        )
        self.assertEqual(service._serial.writes, [b"ACK\n"])

    def test_wait_is_treated_as_implicit_command_ack(self):
        schedule_id = self.create_assigned_schedule()
        service = self.make_service()
        service._active_schedule_id = schedule_id

        service._handle_message("WAIT")

        schedule = database.get_schedule(schedule_id)
        self.assertEqual(schedule["status"], "ALLOWED")
        self.assertEqual(schedule["synced"], 1)

    def test_command_is_retried_after_ten_seconds_until_ack(self):
        schedule_id = database.create_schedule(
            datetime.now().strftime("%Y-%m-%d %H:%M"),
            3600,
        )
        service = self.make_service()

        service._process_schedules()
        self.assertEqual(service._serial.writes, [b"00003600\n"])

        service._last_schedule_check_at = 0
        service._last_transmit_at = (
            time.monotonic() - service.ack_retry_seconds
        )
        service._process_schedules()
        self.assertEqual(
            service._serial.writes,
            [b"00003600\n", b"00003600\n"],
        )

        service._handle_message("ACK")
        service._last_schedule_check_at = 0
        service._last_transmit_at = (
            time.monotonic() - service.ack_retry_seconds
        )
        service._process_schedules()

        self.assertEqual(
            service._serial.writes,
            [b"00003600\n", b"00003600\n"],
        )
        self.assertEqual(
            database.get_schedule(schedule_id)["status"],
            "ALLOWED",
        )

    def test_scheduler_is_blocked_until_system_time_is_set(self):
        schedule_id = database.create_schedule(
            datetime.now().strftime("%Y-%m-%d %H:%M"),
            3600,
        )
        service = UartService(is_time_ready=lambda: False)
        service._serial = FakeSerial()

        service._process_schedules()

        self.assertEqual(service._serial.writes, [])
        schedule = database.get_schedule(schedule_id)
        self.assertEqual(schedule["status"], "WAITING")
        self.assertIsNone(schedule["x_coordinate"])

    def test_missing_result_becomes_missed_after_allowed_time(self):
        schedule_id = database.create_schedule(
            "2000-01-01 09:00",
            60,
        )
        database.assign_coordinate(schedule_id, 0, 0)
        database.mark_synced(schedule_id, "2000-01-01 09:00:01")

        service = self.make_service()
        service._process_schedules()

        self.assertEqual(
            database.get_schedule(schedule_id)["status"],
            "MISSED",
        )
        self.assertEqual(database.get_current_coordinate(), (0, 0))

    def test_never_transmitted_old_schedule_expires_safely(self):
        schedule_id = database.create_schedule(
            "2000-01-01 09:00",
            60,
        )
        database.assign_coordinate(schedule_id, 0, 0)

        database.expire_unstarted_schedules(
            datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        )

        self.assertEqual(
            database.get_schedule(schedule_id)["status"],
            "MISSED",
        )
        self.assertEqual(database.get_current_coordinate(), (0, 0))

    def test_smartphone_time_is_valid_only_for_current_boot(self):
        boot_id_path = Path(self.temp_dir.name) / "boot_id"
        boot_id_path.write_text("boot-one\n", encoding="ascii")
        timestamp_ms = int(
            datetime(
                2026,
                7,
                26,
                9,
                0,
                tzinfo=timezone.utc,
            ).timestamp()
            * 1000
        )

        with (
            patch.object(
                system_time_service,
                "BOOT_ID_PATH",
                boot_id_path,
            ),
            patch.object(
                system_time_service,
                "_run_time_helper",
            ) as run_helper,
        ):
            result = (
                system_time_service.set_system_time_from_smartphone(
                    timestamp_ms,
                    "Asia/Seoul",
                )
            )

            self.assertTrue(result["configured"])
            self.assertTrue(
                system_time_service.is_system_time_configured()
            )
            run_helper.assert_called_once()

            boot_id_path.write_text(
                "boot-two\n",
                encoding="ascii",
            )
            self.assertFalse(
                system_time_service.is_system_time_configured()
            )

    def test_invalid_smartphone_time_is_rejected(self):
        with self.assertRaises(system_time_service.SystemTimeError):
            system_time_service.set_system_time_from_smartphone(
                0,
                "Asia/Seoul",
            )

    def test_coordinate_14_resets_and_runs_exhausted_callback(self):
        callback_count = []

        conn = database.connect_db()
        conn.execute(
            """
            UPDATE device_state
            SET current_x = 1,
                current_y = 4
            WHERE singleton = 1
            """
        )
        conn.commit()
        conn.close()

        schedule_id = self.create_assigned_schedule((1, 4))
        service = self.make_service(
            on_blister_exhausted=lambda: callback_count.append(1)
        )
        service._active_schedule_id = schedule_id

        service._handle_message("ACK")
        service._handle_message("141")

        self.assertEqual(database.get_current_coordinate(), (0, 0))
        self.assertEqual(callback_count, [1])

    def test_reset_deletes_schedules_and_resets_coordinate(self):
        self.create_assigned_schedule()
        database.advance_coordinate("2099-01-01 09:01:00")

        database.reset_schedules_and_position(
            "2099-01-01 09:02:00"
        )

        self.assertEqual(database.get_schedules(), [])
        self.assertEqual(database.get_current_coordinate(), (0, 0))

    def test_delete_schedule_removes_only_selected_schedule(self):
        first_schedule_id = database.create_schedule(
            "2099-01-01 09:00",
            3600,
        )
        second_schedule_id = database.create_schedule(
            "2099-01-01 10:00",
            3600,
        )

        self.assertTrue(database.delete_schedule(first_schedule_id))
        self.assertFalse(database.delete_schedule(first_schedule_id))
        self.assertIsNone(database.get_schedule(first_schedule_id))
        self.assertIsNotNone(database.get_schedule(second_schedule_id))
        self.assertEqual(database.get_current_coordinate(), (0, 0))

    def test_cancel_schedule_only_clears_matching_active_schedule(self):
        active_schedule_id = self.create_assigned_schedule()
        other_schedule_id = database.create_schedule(
            "2099-01-01 10:00",
            3600,
        )
        service = self.make_service()
        service._active_schedule_id = active_schedule_id
        service._last_transmit_at = 123

        self.assertFalse(service.cancel_schedule(other_schedule_id))
        self.assertEqual(
            service.get_status()["active_schedule_id"],
            active_schedule_id,
        )
        self.assertTrue(service.cancel_schedule(active_schedule_id))
        self.assertIsNone(service.get_status()["active_schedule_id"])
        self.assertIsNone(service._last_transmit_at)

    def test_legacy_database_is_migrated_without_losing_rows(self):
        database.DB_PATH.unlink()
        conn = sqlite3.connect(database.DB_PATH)
        conn.execute(
            """
            CREATE TABLE schedules (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                pack_id INTEGER NOT NULL,
                slot INTEGER NOT NULL,
                scheduled_at TEXT NOT NULL,
                status TEXT NOT NULL,
                dispensed_at TEXT,
                synced INTEGER NOT NULL DEFAULT 0
            )
            """
        )
        conn.execute(
            """
            INSERT INTO schedules (
                pack_id,
                slot,
                scheduled_at,
                status,
                synced
            )
            VALUES (1, 1, '2099-01-01 09:00', 'WAITING', 0)
            """
        )
        conn.commit()
        conn.close()

        database.init_db()

        schedules = database.get_schedules()
        self.assertEqual(len(schedules), 1)
        self.assertEqual(schedules[0]["scheduled_at"], "2099-01-01 09:00")
        self.assertEqual(schedules[0]["allowed_seconds"], 3600)
        self.assertIsNone(schedules[0]["x_coordinate"])

    def test_four_digit_seconds_schema_expands_without_losing_data(self):
        database.DB_PATH.unlink()
        conn = sqlite3.connect(database.DB_PATH)
        conn.execute(
            """
            CREATE TABLE schedules (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                scheduled_at TEXT NOT NULL,
                allowed_seconds INTEGER NOT NULL
                    CHECK(allowed_seconds BETWEEN 1 AND 9999),
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
                scheduled_at,
                allowed_seconds,
                status,
                x_coordinate,
                y_coordinate,
                synced,
                command_sent_at,
                ack_received_at
            )
            VALUES (
                '2099-01-01 09:00',
                9999,
                'ALLOWED',
                1,
                4,
                1,
                '2099-01-01 09:00:00',
                '2099-01-01 09:00:01'
            )
            """
        )
        conn.commit()
        conn.close()

        database.init_db()

        schedules = database.get_schedules()
        self.assertEqual(len(schedules), 1)
        self.assertEqual(schedules[0]["allowed_seconds"], 9999)
        self.assertEqual(schedules[0]["x_coordinate"], 1)
        self.assertEqual(schedules[0]["y_coordinate"], 4)

        new_schedule_id = database.create_schedule(
            "2099-01-02 09:00",
            86400,
        )
        self.assertEqual(new_schedule_id, 2)


if __name__ == "__main__":
    unittest.main()
