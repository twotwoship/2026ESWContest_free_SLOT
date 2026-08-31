import os
import sqlite3
from datetime import datetime
from pathlib import Path


DEFAULT_DB_PATH = Path(__file__).resolve().parent / "slotguard.db"
DB_PATH = Path(os.environ.get("SLOTGUARD_DB_PATH", DEFAULT_DB_PATH))
MAX_ALLOWED_SECONDS = 999999
MAX_MEDICINE_NAME_LENGTH = 30
MIN_VOICE_REPEAT = 0
MAX_VOICE_REPEAT = 10
DEFAULT_VOICE_REPEAT = 1
MIN_VOLUME_STEP = 0
MAX_VOLUME_STEP = 10
DEFAULT_VOLUME_STEP = 5
LEGACY_SLOT_COORDINATE_PATH = (
    (0, 0),
    (0, 1),
    (0, 2),
    (0, 3),
    (0, 4),
    (1, 0),
    (1, 1),
    (1, 2),
    (1, 3),
    (1, 4),
)
SLOT_COORDINATE_PATH = (
    (0, 0),
    (0, 1),
    (0, 2),
    (0, 3),
    (0, 4),
    (1, 4),
    (1, 3),
    (1, 2),
    (1, 1),
    (1, 0),
)
SLOT_COORDINATE_PATHS = {
    1: LEGACY_SLOT_COORDINATE_PATH,
    2: SLOT_COORDINATE_PATH,
}

ACTIVE_STATUSES = {
    "MOVING",
    "READY_TO_DISPENSE",
    "DISPENSING",
    "EMPTY_SLOT_CONFIRM",
}
TERMINAL_STATUSES = {
    "DISPENSED",
    "FAILED",
    "MANUALLY_COMPLETED",
    "MISSED",
    "COMM_ERROR",
}
SCHEDULE_STATUSES = {"SCHEDULED", *ACTIVE_STATUSES, *TERMINAL_STATUSES}
ACK_REQUIRED_STATUSES = {"FAILED", "COMM_ERROR"}

SCHEDULE_COLUMNS = {
    "id",
    "medicine_name",
    "scheduled_at",
    "allowed_seconds",
    "status",
    "x_coordinate",
    "y_coordinate",
    "move_request_id",
    "move_sent_at",
    "move_ack_at",
    "ready_at",
    "dispense_request_id",
    "dispense_sent_at",
    "dispense_ack_at",
    "result_at",
    "completed_at",
    "error_code",
    "acknowledged_at",
}

SCHEDULE_SELECT = """
    SELECT
        id,
        medicine_name,
        scheduled_at,
        allowed_seconds,
        status,
        x_coordinate,
        y_coordinate,
        move_request_id,
        move_allowed_seconds,
        move_sent_at,
        move_ack_at,
        ready_at,
        dispense_request_id,
        dispense_sent_at,
        dispense_ack_at,
        result_at,
        completed_at,
        error_code,
        acknowledged_at,
        timeout_requested_at,
        timeout_sent_at,
        timeout_ack_at,
        home_request_id,
        home_requested_at,
        home_move_sent_at,
        home_move_ack_at,
        home_ready_at,
        home_timeout_sent_at,
        home_timeout_ack_at,
        home_error_code
    FROM schedules
"""


class DuplicateScheduleError(ValueError):
    pass


class ActiveDoseError(ValueError):
    pass


def connect_db():
    conn = sqlite3.connect(DB_PATH, timeout=5)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    return conn


def _create_schedules_table(conn):
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS schedules (
            id                    INTEGER PRIMARY KEY AUTOINCREMENT,
            medicine_name         TEXT NOT NULL,
            scheduled_at          TEXT NOT NULL,
            allowed_seconds       INTEGER NOT NULL DEFAULT 3600
                                  CHECK(allowed_seconds BETWEEN 1 AND 999999),
            status                TEXT NOT NULL DEFAULT 'SCHEDULED'
                                  CHECK(status IN (
                                      'SCHEDULED',
                                      'MOVING',
                                      'READY_TO_DISPENSE',
                                      'DISPENSING',
                                      'EMPTY_SLOT_CONFIRM',
                                      'DISPENSED',
                                      'FAILED',
                                      'MANUALLY_COMPLETED',
                                      'MISSED',
                                      'COMM_ERROR'
                                  )),
            x_coordinate          INTEGER CHECK(x_coordinate BETWEEN 0 AND 1),
            y_coordinate          INTEGER CHECK(y_coordinate BETWEEN 0 AND 4),
            move_request_id       TEXT,
            move_allowed_seconds  INTEGER
                                  CHECK(move_allowed_seconds BETWEEN 1 AND 999999),
            move_sent_at          TEXT,
            move_ack_at           TEXT,
            ready_at              TEXT,
            dispense_request_id   TEXT,
            dispense_sent_at      TEXT,
            dispense_ack_at       TEXT,
            result_at             TEXT,
            completed_at          TEXT,
            error_code            TEXT,
            acknowledged_at       TEXT,
            timeout_requested_at  TEXT,
            timeout_sent_at       TEXT,
            timeout_ack_at        TEXT,
            home_request_id       TEXT,
            home_requested_at     TEXT,
            home_move_sent_at     TEXT,
            home_move_ack_at      TEXT,
            home_ready_at         TEXT,
            home_timeout_sent_at  TEXT,
            home_timeout_ack_at   TEXT,
            home_error_code       TEXT,

            CHECK(length(medicine_name) BETWEEN 1 AND 30),
            CHECK(
                (x_coordinate IS NULL AND y_coordinate IS NULL)
                OR
                (x_coordinate IS NOT NULL AND y_coordinate IS NOT NULL)
            )
        )
        """
    )


def _ensure_schedule_retry_columns(conn):
    columns = {
        row["name"]
        for row in conn.execute("PRAGMA table_info(schedules)").fetchall()
    }
    if "move_allowed_seconds" not in columns:
        conn.execute(
            "ALTER TABLE schedules "
            "ADD COLUMN move_allowed_seconds INTEGER "
            "CHECK(move_allowed_seconds BETWEEN 1 AND 999999)"
        )
    for column_name in (
        "timeout_requested_at",
        "timeout_sent_at",
        "timeout_ack_at",
        "home_request_id",
        "home_requested_at",
        "home_move_sent_at",
        "home_move_ack_at",
        "home_ready_at",
        "home_timeout_sent_at",
        "home_timeout_ack_at",
        "home_error_code",
    ):
        if column_name not in columns:
            conn.execute(
                f"ALTER TABLE schedules ADD COLUMN {column_name} TEXT"
            )
    conn.execute(
        "UPDATE schedules SET move_allowed_seconds = allowed_seconds "
        "WHERE move_request_id IS NOT NULL AND move_allowed_seconds IS NULL"
    )


def _legacy_value(row, columns, name, default=None):
    return row[name] if name in columns else default


def _map_legacy_status(status):
    return {
        "WAITING": "SCHEDULED",
        "ALLOWED": "READY_TO_DISPENSE",
    }.get(status, status if status in SCHEDULE_STATUSES else "SCHEDULED")


def _migrate_schedules(conn):
    columns = {
        row["name"]
        for row in conn.execute("PRAGMA table_info(schedules)").fetchall()
    }
    legacy_rows = conn.execute(
        "SELECT * FROM schedules ORDER BY id"
    ).fetchall()

    conn.execute("ALTER TABLE schedules RENAME TO schedules_previous")
    _create_schedules_table(conn)

    for row in legacy_rows:
        status = _map_legacy_status(_legacy_value(row, columns, "status"))
        medicine_name = (
            _legacy_value(row, columns, "medicine_name", "등록된 약")
            or "등록된 약"
        )[:MAX_MEDICINE_NAME_LENGTH]
        legacy_dispensed_at = _legacy_value(
            row,
            columns,
            "dispensed_at",
        )
        legacy_command_at = _legacy_value(
            row,
            columns,
            "command_sent_at",
        )
        legacy_ack_at = _legacy_value(
            row,
            columns,
            "ack_received_at",
        )

        conn.execute(
            """
            INSERT INTO schedules (
                id,
                medicine_name,
                scheduled_at,
                allowed_seconds,
                status,
                x_coordinate,
                y_coordinate,
                move_request_id,
                move_sent_at,
                move_ack_at,
                ready_at,
                dispense_request_id,
                dispense_sent_at,
                dispense_ack_at,
                result_at,
                completed_at,
                error_code,
                acknowledged_at,
                timeout_requested_at,
                timeout_sent_at,
                timeout_ack_at,
                home_request_id,
                home_requested_at,
                home_move_sent_at,
                home_move_ack_at,
                home_ready_at,
                home_timeout_sent_at,
                home_timeout_ack_at,
                home_error_code
            )
            VALUES (
                ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
                ?, ?, ?, ?, ?, ?, ?, ?
            )
            """,
            (
                _legacy_value(row, columns, "id"),
                medicine_name,
                _legacy_value(row, columns, "scheduled_at"),
                _legacy_value(row, columns, "allowed_seconds", 3600),
                status,
                _legacy_value(row, columns, "x_coordinate"),
                _legacy_value(row, columns, "y_coordinate"),
                _legacy_value(row, columns, "move_request_id"),
                _legacy_value(row, columns, "move_sent_at", legacy_command_at),
                _legacy_value(row, columns, "move_ack_at", legacy_ack_at),
                _legacy_value(
                    row,
                    columns,
                    "ready_at",
                    legacy_ack_at if status == "READY_TO_DISPENSE" else None,
                ),
                _legacy_value(row, columns, "dispense_request_id"),
                _legacy_value(row, columns, "dispense_sent_at"),
                _legacy_value(row, columns, "dispense_ack_at"),
                _legacy_value(row, columns, "result_at", legacy_dispensed_at),
                _legacy_value(
                    row,
                    columns,
                    "completed_at",
                    legacy_dispensed_at,
                ),
                _legacy_value(row, columns, "error_code"),
                _legacy_value(row, columns, "acknowledged_at"),
                _legacy_value(row, columns, "timeout_requested_at"),
                _legacy_value(row, columns, "timeout_sent_at"),
                _legacy_value(row, columns, "timeout_ack_at"),
                _legacy_value(row, columns, "home_request_id"),
                _legacy_value(row, columns, "home_requested_at"),
                _legacy_value(row, columns, "home_move_sent_at"),
                _legacy_value(row, columns, "home_move_ack_at"),
                _legacy_value(row, columns, "home_ready_at"),
                _legacy_value(row, columns, "home_timeout_sent_at"),
                _legacy_value(row, columns, "home_timeout_ack_at"),
                _legacy_value(row, columns, "home_error_code"),
            ),
        )

    conn.execute("DROP TABLE schedules_previous")


def _ensure_device_state(conn):
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS device_state (
            singleton           INTEGER PRIMARY KEY CHECK(singleton = 1),
            current_x           INTEGER NOT NULL DEFAULT 0
                                CHECK(current_x BETWEEN 0 AND 1),
            current_y           INTEGER NOT NULL DEFAULT 0
                                CHECK(current_y BETWEEN 0 AND 4),
            blister_exhausted   INTEGER NOT NULL DEFAULT 0
                                CHECK(blister_exhausted IN (0, 1)),
            coordinate_path_version INTEGER NOT NULL DEFAULT 2
                                CHECK(coordinate_path_version IN (1, 2)),
            updated_at          TEXT
        )
        """
    )
    columns = {
        row["name"]
        for row in conn.execute("PRAGMA table_info(device_state)").fetchall()
    }
    if "blister_exhausted" not in columns:
        conn.execute(
            "ALTER TABLE device_state "
            "ADD COLUMN blister_exhausted INTEGER NOT NULL DEFAULT 0"
        )
    if "coordinate_path_version" not in columns:
        conn.execute(
            "ALTER TABLE device_state "
            "ADD COLUMN coordinate_path_version INTEGER NOT NULL DEFAULT 1 "
            "CHECK(coordinate_path_version IN (1, 2))"
        )

    conn.execute(
        """
        INSERT OR IGNORE INTO device_state (
            singleton,
            current_x,
            current_y,
            blister_exhausted
        )
        VALUES (1, 0, 0, 0)
        """
    )
def _ensure_device_settings(conn):
    table = conn.execute(
        """
        SELECT sql FROM sqlite_master
        WHERE type = 'table' AND name = 'device_settings'
        """
    ).fetchone()

    if table is not None and "voice_repeat BETWEEN 0 AND 10" not in table["sql"]:
        columns = {
            row["name"]
            for row in conn.execute(
                "PRAGMA table_info(device_settings)"
            ).fetchall()
        }
        row = conn.execute(
            "SELECT * FROM device_settings WHERE singleton = 1"
        ).fetchone()
        voice_repeat = DEFAULT_VOICE_REPEAT
        volume_step = DEFAULT_VOLUME_STEP
        updated_at = None
        if row is not None:
            if "voice_repeat" in columns:
                voice_repeat = max(
                    MIN_VOICE_REPEAT,
                    min(MAX_VOICE_REPEAT, int(row["voice_repeat"])),
                )
            if "volume_step" in columns:
                volume_step = max(
                    MIN_VOLUME_STEP,
                    min(MAX_VOLUME_STEP, int(row["volume_step"])),
                )
            elif "volume_level" in columns:
                volume_step = {
                    "mute": 0,
                    "low": 3,
                    "medium": 5,
                    "high": 8,
                }.get(row["volume_level"], DEFAULT_VOLUME_STEP)
            if "updated_at" in columns:
                updated_at = row["updated_at"]

        conn.execute(
            "ALTER TABLE device_settings RENAME TO device_settings_previous"
        )
        conn.execute(
            """
            CREATE TABLE device_settings (
                singleton       INTEGER PRIMARY KEY CHECK(singleton = 1),
                voice_repeat    INTEGER NOT NULL DEFAULT 1
                                CHECK(voice_repeat BETWEEN 0 AND 10),
                volume_step     INTEGER NOT NULL DEFAULT 5
                                CHECK(volume_step BETWEEN 0 AND 10),
                updated_at      TEXT
            )
            """
        )
        conn.execute(
            """
            INSERT INTO device_settings (
                singleton, voice_repeat, volume_step, updated_at
            ) VALUES (1, ?, ?, ?)
            """,
            (voice_repeat, volume_step, updated_at),
        )
        conn.execute("DROP TABLE device_settings_previous")

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS device_settings (
            singleton       INTEGER PRIMARY KEY CHECK(singleton = 1),
            voice_repeat    INTEGER NOT NULL DEFAULT 1
                            CHECK(voice_repeat BETWEEN 0 AND 10),
            volume_step     INTEGER NOT NULL DEFAULT 5
                            CHECK(volume_step BETWEEN 0 AND 10),
            updated_at      TEXT
        )
        """
    )
    columns = {
        row["name"]
        for row in conn.execute(
            "PRAGMA table_info(device_settings)"
        ).fetchall()
    }
    if "volume_step" not in columns:
        conn.execute(
            "ALTER TABLE device_settings "
            "ADD COLUMN volume_step INTEGER NOT NULL DEFAULT 5 "
            "CHECK(volume_step BETWEEN 0 AND 10)"
        )
        if "volume_level" in columns:
            conn.execute(
                """
                UPDATE device_settings
                SET volume_step = CASE volume_level
                    WHEN 'mute' THEN 0
                    WHEN 'low' THEN 3
                    WHEN 'medium' THEN 5
                    WHEN 'high' THEN 8
                    ELSE 5
                END
                """
            )

    conn.execute(
        """
        INSERT OR IGNORE INTO device_settings (
            singleton,
            voice_repeat,
            volume_step
        )
        VALUES (1, 1, 5)
        """
    )


def init_db():
    conn = connect_db()

    try:
        # Keep event_log foreign keys pointing at the rebuilt schedules table
        # when a status CHECK constraint requires a table migration.
        conn.execute("PRAGMA foreign_keys = OFF")
        conn.execute("PRAGMA legacy_alter_table = ON")
        conn.execute("BEGIN IMMEDIATE")
        table_exists = conn.execute(
            """
            SELECT 1
            FROM sqlite_master
            WHERE type = 'table' AND name = 'schedules'
            """
        ).fetchone()

        if table_exists:
            columns = {
                row["name"]
                for row in conn.execute(
                    "PRAGMA table_info(schedules)"
                ).fetchall()
            }
            table_sql = conn.execute(
                "SELECT sql FROM sqlite_master "
                "WHERE type = 'table' AND name = 'schedules'"
            ).fetchone()["sql"]
            if (
                not SCHEDULE_COLUMNS.issubset(columns)
                or "EMPTY_SLOT_CONFIRM" not in table_sql
            ):
                _migrate_schedules(conn)
        else:
            _create_schedules_table(conn)

        _ensure_schedule_retry_columns(conn)

        _ensure_device_state(conn)

        _ensure_device_settings(conn)

        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS request_sequence (
                singleton       INTEGER PRIMARY KEY CHECK(singleton = 1),
                last_value      INTEGER NOT NULL DEFAULT 0
            )
            """
        )
        conn.execute(
            """
            INSERT OR IGNORE INTO request_sequence (singleton, last_value)
            VALUES (1, 0)
            """
        )

        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS event_log (
                id              INTEGER PRIMARY KEY AUTOINCREMENT,
                schedule_id     INTEGER,
                event_type      TEXT NOT NULL,
                request_id      TEXT,
                detail          TEXT,
                created_at      TEXT NOT NULL,
                FOREIGN KEY(schedule_id) REFERENCES schedules(id)
                    ON DELETE SET NULL
            )
            """
        )

        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS system_time_state (
                singleton        INTEGER PRIMARY KEY CHECK(singleton = 1),
                boot_id          TEXT,
                configured_at    TEXT,
                source_timezone  TEXT
            )
            """
        )
        conn.execute(
            """
            INSERT OR IGNORE INTO system_time_state (singleton)
            VALUES (1)
            """
        )

        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS system_reset_state (
                singleton       INTEGER PRIMARY KEY CHECK(singleton = 1),
                request_id      TEXT,
                requested_at    TEXT,
                sent_at         TEXT,
                acknowledged_at TEXT,
                completed_at    TEXT,
                error_code      TEXT
            )
            """
        )
        conn.execute(
            """
            INSERT OR IGNORE INTO system_reset_state (singleton)
            VALUES (1)
            """
        )

        conn.execute(
            """
            CREATE INDEX IF NOT EXISTS idx_schedules_due
            ON schedules(status, scheduled_at, id)
            """
        )
        conn.execute(
            """
            CREATE INDEX IF NOT EXISTS idx_event_log_created
            ON event_log(created_at, id)
            """
        )
        conn.execute("PRAGMA user_version = 10")
        conn.commit()

    except Exception:
        conn.rollback()
        raise

    finally:
        conn.execute("PRAGMA legacy_alter_table = OFF")
        conn.execute("PRAGMA foreign_keys = ON")
        conn.close()


def _validate_medicine_name(medicine_name):
    if not isinstance(medicine_name, str):
        raise ValueError("medicine_name must be text")

    medicine_name = medicine_name.strip()
    if not medicine_name or len(medicine_name) > MAX_MEDICINE_NAME_LENGTH:
        raise ValueError(
            f"medicine_name must be 1-{MAX_MEDICINE_NAME_LENGTH} characters"
        )
    return medicine_name


def create_schedule(scheduled_at, medicine_name="등록된 약", allowed_seconds=3600):
    if isinstance(medicine_name, int) and allowed_seconds == 3600:
        allowed_seconds = medicine_name
        medicine_name = "등록된 약"
    medicine_name = _validate_medicine_name(medicine_name)
    if allowed_seconds < 1 or allowed_seconds > MAX_ALLOWED_SECONDS:
        raise ValueError(
            "allowed_seconds must be between "
            f"1 and {MAX_ALLOWED_SECONDS}"
        )

    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        duplicate = conn.execute(
            "SELECT id FROM schedules WHERE scheduled_at = ?",
            (scheduled_at,),
        ).fetchone()
        if duplicate:
            raise DuplicateScheduleError(
                "동일한 날짜와 시각에는 한 건만 예약할 수 있습니다."
            )

        cursor = conn.execute(
            """
            INSERT INTO schedules (
                medicine_name,
                scheduled_at,
                allowed_seconds
            )
            VALUES (?, ?, ?)
            """,
            (medicine_name, scheduled_at, allowed_seconds),
        )
        conn.commit()
        return cursor.lastrowid
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def get_schedules():
    conn = connect_db()
    try:
        rows = conn.execute(
            SCHEDULE_SELECT + " ORDER BY scheduled_at, id"
        ).fetchall()
        return [dict(row) for row in rows]
    finally:
        conn.close()


def get_schedule(schedule_id):
    conn = connect_db()
    try:
        row = conn.execute(
            SCHEDULE_SELECT + " WHERE id = ?",
            (schedule_id,),
        ).fetchone()
        return dict(row) if row else None
    finally:
        conn.close()


def get_schedule_by_dispense_request(request_id):
    conn = connect_db()
    try:
        row = conn.execute(
            SCHEDULE_SELECT + " WHERE dispense_request_id = ?",
            (request_id,),
        ).fetchone()
        return dict(row) if row else None
    finally:
        conn.close()


def get_timeout_schedule_by_request(request_id):
    conn = connect_db()
    try:
        row = conn.execute(
            SCHEDULE_SELECT
            + " WHERE move_request_id = ? "
            "AND timeout_requested_at IS NOT NULL "
            "ORDER BY id DESC LIMIT 1",
            (request_id,),
        ).fetchone()
        return dict(row) if row else None
    finally:
        conn.close()


def get_pending_timeout():
    conn = connect_db()
    try:
        row = conn.execute(
            SCHEDULE_SELECT
            + " WHERE timeout_requested_at IS NOT NULL "
            "AND timeout_ack_at IS NULL "
            "ORDER BY timeout_requested_at, id LIMIT 1"
        ).fetchone()
        return dict(row) if row else None
    finally:
        conn.close()


def get_home_return_by_request(request_id):
    conn = connect_db()
    try:
        row = conn.execute(
            SCHEDULE_SELECT
            + " WHERE home_request_id = ? ORDER BY id DESC LIMIT 1",
            (request_id,),
        ).fetchone()
        return dict(row) if row else None
    finally:
        conn.close()


def get_pending_home_return():
    conn = connect_db()
    try:
        row = conn.execute(
            SCHEDULE_SELECT
            + " WHERE home_request_id IS NOT NULL "
            "AND home_timeout_ack_at IS NULL "
            "AND home_error_code IS NULL "
            "ORDER BY home_requested_at, id LIMIT 1"
        ).fetchone()
        return dict(row) if row else None
    finally:
        conn.close()


def get_blocking_home_return():
    conn = connect_db()
    try:
        row = conn.execute(
            SCHEDULE_SELECT
            + " WHERE home_request_id IS NOT NULL "
            "AND home_timeout_ack_at IS NULL "
            "ORDER BY home_requested_at, id LIMIT 1"
        ).fetchone()
        return dict(row) if row else None
    finally:
        conn.close()


def is_result_request_processed(request_id):
    conn = connect_db()
    try:
        row = conn.execute(
            """
            SELECT 1
            FROM event_log
            WHERE request_id = ?
              AND event_type IN (
                  'EMPTY_SLOT_CONFIRM_REQUIRED',
                  'EMPTY_BLISTER_SLOT',
                  'DISPENSED',
                  'FAILED',
                  'MANUALLY_COMPLETED'
              )
            LIMIT 1
            """,
            (request_id,),
        ).fetchone()
        return row is not None
    finally:
        conn.close()


def get_next_schedule():
    conn = connect_db()
    try:
        row = conn.execute(
            SCHEDULE_SELECT
            + " WHERE status = 'SCHEDULED' ORDER BY scheduled_at, id LIMIT 1"
        ).fetchone()
        return dict(row) if row else None
    finally:
        conn.close()


def get_active_schedule():
    placeholders = ",".join("?" for _ in ACTIVE_STATUSES)
    conn = connect_db()
    try:
        row = conn.execute(
            SCHEDULE_SELECT
            + f" WHERE status IN ({placeholders}) "
            "ORDER BY scheduled_at, id LIMIT 1",
            tuple(sorted(ACTIVE_STATUSES)),
        ).fetchone()
        return dict(row) if row else None
    finally:
        conn.close()


def get_unacknowledged_result():
    placeholders = ",".join("?" for _ in ACK_REQUIRED_STATUSES)
    conn = connect_db()
    try:
        row = conn.execute(
            SCHEDULE_SELECT
            + f" WHERE status IN ({placeholders}) "
            "AND acknowledged_at IS NULL "
            "ORDER BY COALESCE(completed_at, scheduled_at), id LIMIT 1",
            tuple(sorted(ACK_REQUIRED_STATUSES)),
        ).fetchone()
        return dict(row) if row else None
    finally:
        conn.close()


def get_latest_result():
    placeholders = ",".join("?" for _ in TERMINAL_STATUSES)
    conn = connect_db()
    try:
        row = conn.execute(
            SCHEDULE_SELECT
            + f" WHERE status IN ({placeholders}) "
            "ORDER BY COALESCE(completed_at, scheduled_at) DESC, id DESC LIMIT 1",
            tuple(sorted(TERMINAL_STATUSES)),
        ).fetchone()
        return dict(row) if row else None
    finally:
        conn.close()


def get_recent_records(limit=3):
    placeholders = ",".join("?" for _ in TERMINAL_STATUSES)
    conn = connect_db()
    try:
        rows = conn.execute(
            SCHEDULE_SELECT
            + f" WHERE status IN ({placeholders}) "
            "ORDER BY COALESCE(completed_at, scheduled_at) DESC, id DESC "
            "LIMIT ?",
            (*tuple(sorted(TERMINAL_STATUSES)), limit),
        ).fetchall()
        return [dict(row) for row in rows]
    finally:
        conn.close()


def delete_schedule(schedule_id):
    conn = connect_db()
    try:
        cursor = conn.execute(
            "DELETE FROM schedules WHERE id = ? AND status = 'SCHEDULED'",
            (schedule_id,),
        )
        conn.commit()
        return cursor.rowcount > 0
    finally:
        conn.close()


def get_due_schedule(current_time):
    conn = connect_db()
    try:
        row = conn.execute(
            SCHEDULE_SELECT
            + " WHERE status = 'SCHEDULED' "
            "AND x_coordinate IS NULL AND scheduled_at <= ? "
            "ORDER BY scheduled_at, id LIMIT 1",
            (current_time,),
        ).fetchone()
        return dict(row) if row else None
    finally:
        conn.close()


def assign_coordinate(schedule_id, x_coordinate, y_coordinate):
    conn = connect_db()
    try:
        conn.execute(
            """
            UPDATE schedules
            SET x_coordinate = ?, y_coordinate = ?
            WHERE id = ? AND status = 'SCHEDULED'
              AND x_coordinate IS NULL AND y_coordinate IS NULL
            """,
            (x_coordinate, y_coordinate, schedule_id),
        )
        conn.commit()
        row = conn.execute(
            SCHEDULE_SELECT + " WHERE id = ?",
            (schedule_id,),
        ).fetchone()
        return dict(row) if row else None
    finally:
        conn.close()


def allocate_request_id(conn=None):
    owns_connection = conn is None
    if owns_connection:
        conn = connect_db()
        conn.execute("BEGIN IMMEDIATE")

    try:
        row = conn.execute(
            "SELECT last_value FROM request_sequence WHERE singleton = 1"
        ).fetchone()
        next_value = (int(row["last_value"]) % 0xFFFFFFFF) + 1
        conn.execute(
            "UPDATE request_sequence SET last_value = ? WHERE singleton = 1",
            (next_value,),
        )
        if owns_connection:
            conn.commit()
        return f"{next_value:08X}"
    except Exception:
        if owns_connection:
            conn.rollback()
        raise
    finally:
        if owns_connection:
            conn.close()


def prepare_move(schedule_id, event_time):
    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        row = conn.execute(
            SCHEDULE_SELECT + " WHERE id = ?",
            (schedule_id,),
        ).fetchone()
        if row is None or row["status"] not in {"SCHEDULED", "MOVING"}:
            conn.rollback()
            return None

        request_id = row["move_request_id"] or allocate_request_id(conn)
        conn.execute(
            """
            UPDATE schedules
            SET status = 'MOVING', move_request_id = ?,
                move_allowed_seconds = COALESCE(
                    move_allowed_seconds,
                    allowed_seconds
                )
            WHERE id = ?
            """,
            (request_id, schedule_id),
        )
        _insert_event(
            conn,
            schedule_id,
            "MOVE_PREPARED",
            event_time,
            request_id=request_id,
        )
        conn.commit()
        return get_schedule(schedule_id)
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def record_move_sent(schedule_id, event_time):
    _update_time_field(schedule_id, "move_sent_at", event_time, "MOVE_SENT")


def mark_move_ack(schedule_id, request_id, event_time):
    return _mark_request_time(
        schedule_id,
        "move_request_id",
        request_id,
        "move_ack_at",
        event_time,
        "MOVE_ACK",
    )


def mark_ready(schedule_id, request_id, event_time):
    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        cursor = conn.execute(
            """
            UPDATE schedules
            SET status = 'READY_TO_DISPENSE',
                move_ack_at = COALESCE(move_ack_at, ?),
                ready_at = COALESCE(ready_at, ?)
            WHERE id = ? AND move_request_id = ? AND status = 'MOVING'
            """,
            (event_time, event_time, schedule_id, request_id),
        )
        if cursor.rowcount:
            _insert_event(
                conn,
                schedule_id,
                "READY_TO_DISPENSE",
                event_time,
                request_id=request_id,
            )
        conn.commit()
        return cursor.rowcount > 0
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def prepare_dispense(schedule_id, event_time):
    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        row = conn.execute(
            SCHEDULE_SELECT + " WHERE id = ?",
            (schedule_id,),
        ).fetchone()
        if row is None or row["status"] != "READY_TO_DISPENSE":
            raise ActiveDoseError("현재 약 배출을 시작할 수 없습니다.")

        request_id = row["dispense_request_id"] or allocate_request_id(conn)
        conn.execute(
            """
            UPDATE schedules
            SET status = 'DISPENSING', dispense_request_id = ?
            WHERE id = ?
            """,
            (request_id, schedule_id),
        )
        _insert_event(
            conn,
            schedule_id,
            "DISPENSE_PREPARED",
            event_time,
            request_id=request_id,
        )
        conn.commit()
        return get_schedule(schedule_id)
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def record_dispense_sent(schedule_id, event_time):
    _update_time_field(
        schedule_id,
        "dispense_sent_at",
        event_time,
        "DISPENSE_SENT",
    )


def record_timeout_sent(schedule_id, event_time):
    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        row = conn.execute(
            "SELECT move_request_id FROM schedules "
            "WHERE id = ? AND timeout_requested_at IS NOT NULL "
            "AND timeout_ack_at IS NULL",
            (schedule_id,),
        ).fetchone()
        if row is None:
            conn.rollback()
            return False
        conn.execute(
            "UPDATE schedules "
            "SET timeout_sent_at = COALESCE(timeout_sent_at, ?) "
            "WHERE id = ?",
            (event_time, schedule_id),
        )
        _insert_event(
            conn,
            schedule_id,
            "TIMEOUT_SENT",
            event_time,
            request_id=row["move_request_id"],
        )
        conn.commit()
        return True
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def mark_timeout_ack(request_id, event_time):
    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        row = conn.execute(
            "SELECT id, timeout_ack_at FROM schedules "
            "WHERE move_request_id = ? "
            "AND timeout_requested_at IS NOT NULL "
            "ORDER BY id DESC LIMIT 1",
            (request_id,),
        ).fetchone()
        if row is None:
            conn.rollback()
            return False
        if row["timeout_ack_at"] is None:
            conn.execute(
                "UPDATE schedules SET timeout_ack_at = ? WHERE id = ?",
                (event_time, row["id"]),
            )
            _insert_event(
                conn,
                row["id"],
                "TIMEOUT_ACK",
                event_time,
                request_id=request_id,
            )
        conn.commit()
        return True
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def prepare_home_return(schedule_id, event_time):
    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        schedule = conn.execute(
            SCHEDULE_SELECT + " WHERE id = ?",
            (schedule_id,),
        ).fetchone()
        state = conn.execute(
            "SELECT blister_exhausted FROM device_state WHERE singleton = 1"
        ).fetchone()
        if (
            schedule is None
            or schedule["status"] not in {"DISPENSED", "FAILED"}
            or not state["blister_exhausted"]
        ):
            raise ActiveDoseError("현재 원점 복귀를 시작할 수 없습니다.")

        if schedule["home_request_id"] is None:
            request_id = allocate_request_id(conn)
            conn.execute(
                """
                UPDATE schedules
                SET home_request_id = ?, home_requested_at = ?,
                    home_move_sent_at = NULL, home_move_ack_at = NULL,
                    home_ready_at = NULL, home_timeout_sent_at = NULL,
                    home_timeout_ack_at = NULL, home_error_code = NULL
                WHERE id = ?
                """,
                (request_id, event_time, schedule_id),
            )
            _insert_event(
                conn,
                schedule_id,
                "HOME_RETURN_PREPARED",
                event_time,
                request_id=request_id,
                detail="00",
            )
        conn.commit()
        return get_schedule(schedule_id)
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def record_home_move_sent(schedule_id, event_time):
    conn = connect_db()
    try:
        conn.execute(
            "UPDATE schedules "
            "SET home_move_sent_at = COALESCE(home_move_sent_at, ?) "
            "WHERE id = ? AND home_request_id IS NOT NULL "
            "AND home_timeout_ack_at IS NULL AND home_error_code IS NULL",
            (event_time, schedule_id),
        )
        row = conn.execute(
            "SELECT home_request_id FROM schedules WHERE id = ?",
            (schedule_id,),
        ).fetchone()
        if row is not None:
            _insert_event(
                conn,
                schedule_id,
                "HOME_MOVE_SENT",
                event_time,
                request_id=row["home_request_id"],
                detail="00",
            )
        conn.commit()
    finally:
        conn.close()


def mark_home_move_ack(schedule_id, request_id, event_time):
    conn = connect_db()
    try:
        cursor = conn.execute(
            "UPDATE schedules "
            "SET home_move_ack_at = COALESCE(home_move_ack_at, ?) "
            "WHERE id = ? AND home_request_id = ? "
            "AND home_timeout_ack_at IS NULL AND home_error_code IS NULL",
            (event_time, schedule_id, request_id),
        )
        if cursor.rowcount:
            _insert_event(
                conn,
                schedule_id,
                "HOME_MOVE_ACK",
                event_time,
                request_id=request_id,
            )
        conn.commit()
        return cursor.rowcount > 0
    finally:
        conn.close()


def mark_home_ready(schedule_id, request_id, event_time):
    conn = connect_db()
    try:
        cursor = conn.execute(
            """
            UPDATE schedules
            SET home_move_ack_at = COALESCE(home_move_ack_at, ?),
                home_ready_at = COALESCE(home_ready_at, ?)
            WHERE id = ? AND home_request_id = ?
              AND home_timeout_ack_at IS NULL AND home_error_code IS NULL
            """,
            (event_time, event_time, schedule_id, request_id),
        )
        if cursor.rowcount:
            _insert_event(
                conn,
                schedule_id,
                "HOME_READY",
                event_time,
                request_id=request_id,
                detail="00",
            )
        conn.commit()
        return cursor.rowcount > 0
    finally:
        conn.close()


def record_home_timeout_sent(schedule_id, event_time):
    conn = connect_db()
    try:
        cursor = conn.execute(
            "UPDATE schedules "
            "SET home_timeout_sent_at = COALESCE(home_timeout_sent_at, ?) "
            "WHERE id = ? AND home_ready_at IS NOT NULL "
            "AND home_timeout_ack_at IS NULL AND home_error_code IS NULL",
            (event_time, schedule_id),
        )
        row = conn.execute(
            "SELECT home_request_id FROM schedules WHERE id = ?",
            (schedule_id,),
        ).fetchone()
        if cursor.rowcount and row is not None:
            _insert_event(
                conn,
                schedule_id,
                "HOME_TIMEOUT_SENT",
                event_time,
                request_id=row["home_request_id"],
            )
        conn.commit()
        return cursor.rowcount > 0
    finally:
        conn.close()


def complete_home_return(schedule_id, request_id, event_time):
    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        row = conn.execute(
            "SELECT home_timeout_ack_at FROM schedules "
            "WHERE id = ? AND home_request_id = ? "
            "AND home_ready_at IS NOT NULL AND home_error_code IS NULL",
            (schedule_id, request_id),
        ).fetchone()
        if row is None:
            conn.rollback()
            return False
        if row["home_timeout_ack_at"] is not None:
            conn.commit()
            return False

        conn.execute(
            "UPDATE schedules SET home_timeout_ack_at = ? WHERE id = ?",
            (event_time, schedule_id),
        )
        conn.execute(
            """
            UPDATE device_state
            SET current_x = 0, current_y = 0, updated_at = ?
            WHERE singleton = 1
            """,
            (event_time,),
        )
        _insert_event(
            conn,
            schedule_id,
            "HOME_TIMEOUT_ACK",
            event_time,
            request_id=request_id,
        )
        _insert_event(
            conn,
            schedule_id,
            "HOME_RETURN_COMPLETED",
            event_time,
            request_id=request_id,
            detail="00",
        )
        conn.commit()
        return True
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def mark_home_return_error(schedule_id, request_id, event_time, error_code):
    conn = connect_db()
    try:
        cursor = conn.execute(
            "UPDATE schedules "
            "SET home_error_code = COALESCE(home_error_code, ?) "
            "WHERE id = ? AND home_request_id = ? "
            "AND home_timeout_ack_at IS NULL",
            (error_code, schedule_id, request_id),
        )
        if cursor.rowcount:
            _insert_event(
                conn,
                schedule_id,
                "HOME_RETURN_ERROR",
                event_time,
                request_id=request_id,
                detail=error_code,
            )
        conn.commit()
        return cursor.rowcount > 0
    finally:
        conn.close()


def mark_dispense_ack(schedule_id, request_id, event_time):
    return _mark_request_time(
        schedule_id,
        "dispense_request_id",
        request_id,
        "dispense_ack_at",
        event_time,
        "DISPENSE_ACK",
    )


def complete_dispensed(schedule_id, request_id, event_time):
    return _complete_and_advance(
        schedule_id,
        request_id,
        "DISPENSED",
        event_time,
    )


def complete_manual(schedule_id, event_time):
    return _complete_and_advance(
        schedule_id,
        None,
        "MANUALLY_COMPLETED",
        event_time,
        required_current_status="FAILED",
        excluded_error_code="EMPTY_BLISTER_SLOT",
    )


def request_empty_slot_confirmation(schedule_id, request_id, event_time):
    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        row = conn.execute(
            SCHEDULE_SELECT
            + " WHERE id = ? AND status = 'DISPENSING' "
            "AND dispense_request_id = ?",
            (schedule_id, request_id),
        ).fetchone()
        if row is None:
            conn.rollback()
            return None

        coordinate_detail = f"{row['x_coordinate']}{row['y_coordinate']}"
        conn.execute(
            """
            UPDATE schedules
            SET status = 'EMPTY_SLOT_CONFIRM',
                result_at = COALESCE(result_at, ?),
                error_code = 'EMPTY_SLOT_CONFIRM'
            WHERE id = ?
            """,
            (event_time, schedule_id),
        )
        _insert_event(
            conn,
            schedule_id,
            "EMPTY_SLOT_CONFIRM_REQUIRED",
            event_time,
            request_id=request_id,
            detail=coordinate_detail,
        )
        conn.commit()
        return get_schedule(schedule_id)
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def complete_empty_slot_confirmed_dispensed(
    schedule_id,
    request_id,
    event_time,
):
    return _complete_and_advance(
        schedule_id,
        request_id,
        "DISPENSED",
        event_time,
        required_current_status="EMPTY_SLOT_CONFIRM",
        event_detail="USER_CONFIRMED_AFTER_EMPTY_RESULT",
    )


def continue_after_empty_slot(
    schedule_id,
    request_id,
    event_time,
    remaining_seconds,
):
    if remaining_seconds < 1 or remaining_seconds > MAX_ALLOWED_SECONDS:
        raise ValueError(
            "remaining_seconds must be between "
            f"1 and {MAX_ALLOWED_SECONDS}"
        )

    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        row = conn.execute(
            SCHEDULE_SELECT
            + " WHERE id = ? AND status = 'EMPTY_SLOT_CONFIRM' "
            "AND dispense_request_id = ?",
            (schedule_id, request_id),
        ).fetchone()
        if row is None:
            conn.rollback()
            return None

        coordinate_result = _advance_coordinate_in_transaction(conn, event_time)
        coordinate_detail = f"{row['x_coordinate']}{row['y_coordinate']}"
        _insert_event(
            conn,
            schedule_id,
            "EMPTY_BLISTER_SLOT",
            event_time,
            request_id=request_id,
            detail=coordinate_detail,
        )

        if coordinate_result["blister_exhausted"]:
            conn.execute(
                """
                UPDATE schedules
                SET status = 'FAILED',
                    result_at = COALESCE(result_at, ?),
                    completed_at = ?,
                    error_code = 'EMPTY_BLISTER_SLOT',
                    acknowledged_at = NULL
                WHERE id = ?
                """,
                (event_time, event_time, schedule_id),
            )
            _insert_event(
                conn,
                schedule_id,
                "FAILED",
                event_time,
                request_id=request_id,
                detail="EMPTY_BLISTER_SLOT",
            )
        else:
            move_request_id = allocate_request_id(conn)
            next_x, next_y = coordinate_result["current"]
            conn.execute(
                """
                UPDATE schedules
                SET status = 'MOVING',
                    x_coordinate = ?,
                    y_coordinate = ?,
                    move_request_id = ?,
                    move_allowed_seconds = ?,
                    move_sent_at = NULL,
                    move_ack_at = NULL,
                    ready_at = NULL,
                    dispense_request_id = NULL,
                    dispense_sent_at = NULL,
                    dispense_ack_at = NULL,
                    result_at = NULL,
                    completed_at = NULL,
                    error_code = 'EMPTY_BLISTER_SLOT',
                    acknowledged_at = NULL
                WHERE id = ?
                """,
                (
                    next_x,
                    next_y,
                    move_request_id,
                    remaining_seconds,
                    schedule_id,
                ),
            )
            _insert_event(
                conn,
                schedule_id,
                "MOVE_PREPARED",
                event_time,
                request_id=move_request_id,
                detail="EMPTY_BLISTER_SLOT",
            )

        conn.commit()
        result = dict(coordinate_result)
        result["schedule"] = get_schedule(schedule_id)
        return result
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def resume_after_empty_blister(
    schedule_id,
    event_time,
    remaining_seconds,
):
    if remaining_seconds < 1 or remaining_seconds > MAX_ALLOWED_SECONDS:
        raise ValueError(
            "remaining_seconds must be between "
            f"1 and {MAX_ALLOWED_SECONDS}"
        )

    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        row = conn.execute(
            """
            SELECT current_x, current_y, blister_exhausted
            FROM device_state WHERE singleton = 1
            """
        ).fetchone()
        if row["blister_exhausted"] or (
            row["current_x"],
            row["current_y"],
        ) != (0, 0):
            raise ActiveDoseError("새 블리스터를 먼저 초기화해 주세요.")

        schedule = conn.execute(
            SCHEDULE_SELECT
            + " WHERE id = ? AND status = 'FAILED' "
            "AND error_code = 'EMPTY_BLISTER_SLOT' "
            "AND acknowledged_at IS NULL",
            (schedule_id,),
        ).fetchone()
        if schedule is None:
            raise ActiveDoseError("재개할 빈 슬롯 복약 기록이 없습니다.")

        move_request_id = allocate_request_id(conn)
        conn.execute(
            """
            UPDATE schedules
            SET status = 'MOVING',
                x_coordinate = 0,
                y_coordinate = 0,
                move_request_id = ?,
                move_allowed_seconds = ?,
                move_sent_at = NULL,
                move_ack_at = NULL,
                ready_at = NULL,
                dispense_request_id = NULL,
                dispense_sent_at = NULL,
                dispense_ack_at = NULL,
                result_at = NULL,
                completed_at = NULL,
                error_code = 'EMPTY_BLISTER_SLOT',
                acknowledged_at = NULL
            WHERE id = ?
            """,
            (move_request_id, remaining_seconds, schedule_id),
        )
        _insert_event(
            conn,
            schedule_id,
            "EMPTY_BLISTER_MANUAL_NOT_TAKEN",
            event_time,
            detail="RETRY_FROM_00",
        )
        _insert_event(
            conn,
            schedule_id,
            "MOVE_PREPARED",
            event_time,
            request_id=move_request_id,
            detail="EMPTY_BLISTER_RETRY",
        )
        conn.commit()
        return get_schedule(schedule_id)
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def complete_empty_blister_manual(schedule_id, event_time):
    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        state = conn.execute(
            """
            SELECT current_x, current_y, blister_exhausted
            FROM device_state WHERE singleton = 1
            """
        ).fetchone()
        if state["blister_exhausted"] or (
            state["current_x"],
            state["current_y"],
        ) != (0, 0):
            raise ActiveDoseError("새 블리스터를 먼저 초기화해 주세요.")

        cursor = conn.execute(
            """
            UPDATE schedules
            SET status = 'MANUALLY_COMPLETED',
                completed_at = ?,
                error_code = NULL,
                acknowledged_at = NULL,
                x_coordinate = 0,
                y_coordinate = 0
            WHERE id = ? AND status = 'FAILED'
              AND error_code = 'EMPTY_BLISTER_SLOT'
              AND acknowledged_at IS NULL
            """,
            (event_time, schedule_id),
        )
        if cursor.rowcount == 0:
            conn.rollback()
            return None

        coordinate_result = {
            "previous": (0, 0),
            "current": (0, 0),
            "blister_exhausted": False,
        }
        _insert_event(
            conn,
            schedule_id,
            "EMPTY_BLISTER_MANUAL_TAKEN",
            event_time,
            detail="00_UNCHANGED",
        )
        _insert_event(
            conn,
            schedule_id,
            "MANUALLY_COMPLETED",
            event_time,
            detail="EMPTY_BLISTER_SLOT",
        )
        conn.commit()
        return coordinate_result
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def mark_failed(schedule_id, request_id, event_time, error_code="NO_DROP_DETECTED"):
    return _set_terminal_status(
        schedule_id,
        "FAILED",
        event_time,
        error_code,
        request_id=request_id,
        required_status="DISPENSING",
    )


def mark_missed(schedule_id, event_time, error_code="DOSE_WINDOW_EXPIRED"):
    return _set_terminal_status(
        schedule_id,
        "MISSED",
        event_time,
        error_code,
    )


def mark_missed_and_queue_timeout(
    schedule_id,
    event_time,
    error_code="DOSE_BUTTON_TIMEOUT",
):
    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        row = conn.execute(
            "SELECT move_request_id FROM schedules "
            "WHERE id = ? AND status = 'READY_TO_DISPENSE'",
            (schedule_id,),
        ).fetchone()
        if row is None or row["move_request_id"] is None:
            conn.rollback()
            return None

        conn.execute(
            "UPDATE schedules "
            "SET status = 'MISSED', "
            "result_at = COALESCE(result_at, ?), "
            "completed_at = COALESCE(completed_at, ?), "
            "error_code = ?, "
            "timeout_requested_at = COALESCE(timeout_requested_at, ?) "
            "WHERE id = ? AND status = 'READY_TO_DISPENSE'",
            (event_time, event_time, error_code, event_time, schedule_id),
        )
        _insert_event(
            conn,
            schedule_id,
            "MISSED",
            event_time,
            request_id=row["move_request_id"],
            detail=error_code,
        )
        _insert_event(
            conn,
            schedule_id,
            "TIMEOUT_QUEUED",
            event_time,
            request_id=row["move_request_id"],
            detail=error_code,
        )
        conn.commit()
        return get_schedule(schedule_id)
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def mark_comm_error(schedule_id, event_time, error_code):
    return _set_terminal_status(
        schedule_id,
        "COMM_ERROR",
        event_time,
        error_code,
    )


def acknowledge_result(schedule_id, event_time):
    conn = connect_db()
    try:
        cursor = conn.execute(
            """
            UPDATE schedules
            SET acknowledged_at = COALESCE(acknowledged_at, ?)
            WHERE id = ? AND status IN ('FAILED', 'MISSED', 'COMM_ERROR')
              AND COALESCE(error_code, '') != 'EMPTY_BLISTER_SLOT'
            """,
            (event_time, schedule_id),
        )
        if cursor.rowcount:
            _insert_event(conn, schedule_id, "RESULT_ACKNOWLEDGED", event_time)
        conn.commit()
        return cursor.rowcount > 0
    finally:
        conn.close()


def _update_time_field(schedule_id, field_name, event_time, event_type):
    allowed_fields = {"move_sent_at", "dispense_sent_at"}
    if field_name not in allowed_fields:
        raise ValueError("unsupported time field")
    conn = connect_db()
    try:
        conn.execute(
            f"UPDATE schedules SET {field_name} = COALESCE({field_name}, ?) "
            "WHERE id = ?",
            (event_time, schedule_id),
        )
        row = conn.execute(
            "SELECT move_request_id, dispense_request_id FROM schedules "
            "WHERE id = ?",
            (schedule_id,),
        ).fetchone()
        request_id = (
            row["move_request_id"]
            if field_name == "move_sent_at"
            else row["dispense_request_id"]
        )
        _insert_event(
            conn,
            schedule_id,
            event_type,
            event_time,
            request_id=request_id,
        )
        conn.commit()
    finally:
        conn.close()


def _mark_request_time(
    schedule_id,
    request_field,
    request_id,
    time_field,
    event_time,
    event_type,
):
    allowed = {
        ("move_request_id", "move_ack_at"),
        ("dispense_request_id", "dispense_ack_at"),
    }
    if (request_field, time_field) not in allowed:
        raise ValueError("unsupported request field")
    conn = connect_db()
    try:
        cursor = conn.execute(
            f"UPDATE schedules SET {time_field} = COALESCE({time_field}, ?) "
            f"WHERE id = ? AND {request_field} = ?",
            (event_time, schedule_id, request_id),
        )
        if cursor.rowcount:
            _insert_event(
                conn,
                schedule_id,
                event_type,
                event_time,
                request_id=request_id,
            )
        conn.commit()
        return cursor.rowcount > 0
    finally:
        conn.close()


def _set_terminal_status(
    schedule_id,
    status,
    event_time,
    error_code,
    request_id=None,
    required_status=None,
):
    if status not in TERMINAL_STATUSES:
        raise ValueError("terminal status required")
    conn = connect_db()
    try:
        clauses = ["id = ?"]
        params = [status, event_time, event_time, error_code, schedule_id]
        if request_id is not None:
            clauses.append("dispense_request_id = ?")
            params.append(request_id)
        if required_status is not None:
            clauses.append("status = ?")
            params.append(required_status)

        cursor = conn.execute(
            "UPDATE schedules SET status = ?, result_at = COALESCE(result_at, ?), "
            "completed_at = COALESCE(completed_at, ?), error_code = ? WHERE "
            + " AND ".join(clauses),
            tuple(params),
        )
        if cursor.rowcount:
            _insert_event(
                conn,
                schedule_id,
                status,
                event_time,
                request_id=request_id,
                detail=error_code,
            )
        conn.commit()
        return cursor.rowcount > 0
    finally:
        conn.close()


def _complete_and_advance(
    schedule_id,
    request_id,
    status,
    event_time,
    required_current_status="DISPENSING",
    excluded_error_code=None,
    event_detail=None,
):
    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        clauses = ["id = ?", "status = ?"]
        params = [status, event_time, event_time, schedule_id, required_current_status]
        if request_id is not None:
            clauses.append("dispense_request_id = ?")
            params.append(request_id)
        if excluded_error_code is not None:
            clauses.append("COALESCE(error_code, '') != ?")
            params.append(excluded_error_code)

        cursor = conn.execute(
            "UPDATE schedules SET status = ?, result_at = COALESCE(result_at, ?), "
            "completed_at = ?, error_code = NULL WHERE "
            + " AND ".join(clauses),
            tuple(params),
        )
        if cursor.rowcount == 0:
            conn.rollback()
            return None

        coordinate_result = _advance_coordinate_in_transaction(conn, event_time)
        _insert_event(
            conn,
            schedule_id,
            status,
            event_time,
            request_id=request_id,
            detail=event_detail,
        )
        conn.commit()
        return coordinate_result
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def _advance_coordinate_in_transaction(conn, event_time):
    row = conn.execute(
        """
        SELECT current_x, current_y, blister_exhausted, coordinate_path_version
        FROM device_state WHERE singleton = 1
        """
    ).fetchone()
    current_x = row["current_x"]
    current_y = row["current_y"]
    blister_exhausted = bool(row["blister_exhausted"])
    coordinate_path = SLOT_COORDINATE_PATHS[row["coordinate_path_version"]]

    if blister_exhausted:
        raise ActiveDoseError("새 블리스터 초기화가 필요합니다.")

    current_coordinate = (current_x, current_y)
    current_index = coordinate_path.index(current_coordinate)
    exhausted_now = current_index == len(coordinate_path) - 1
    if exhausted_now:
        next_x, next_y = current_x, current_y
    else:
        next_x, next_y = coordinate_path[current_index + 1]

    conn.execute(
        """
        UPDATE device_state
        SET current_x = ?, current_y = ?, blister_exhausted = ?, updated_at = ?
        WHERE singleton = 1
        """,
        (next_x, next_y, int(exhausted_now), event_time),
    )
    return {
        "previous": (current_x, current_y),
        "current": (next_x, next_y),
        "blister_exhausted": exhausted_now,
    }


def advance_coordinate(event_time):
    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        result = _advance_coordinate_in_transaction(conn, event_time)
        conn.commit()
        return result
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def get_device_state():
    conn = connect_db()
    try:
        row = conn.execute(
            """
            SELECT current_x, current_y, blister_exhausted,
                   coordinate_path_version, updated_at
            FROM device_state WHERE singleton = 1
            """
        ).fetchone()
        return dict(row)
    finally:
        conn.close()


def get_current_coordinate():
    state = get_device_state()
    return state["current_x"], state["current_y"]


def get_used_coordinates():
    state = get_device_state()
    coordinate_path = SLOT_COORDINATE_PATHS[
        state["coordinate_path_version"]
    ]
    if state["blister_exhausted"]:
        count = len(coordinate_path)
    else:
        current = (state["current_x"], state["current_y"])
        count = coordinate_path.index(current)
    return list(coordinate_path[:count])


def reset_blister(event_time):
    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        active = conn.execute(
            """
            SELECT id FROM schedules
            WHERE status IN (
                'MOVING',
                'READY_TO_DISPENSE',
                'DISPENSING',
                'EMPTY_SLOT_CONFIRM'
            )
            LIMIT 1
            """
        ).fetchone()
        if active:
            raise ActiveDoseError("복약 처리 중에는 초기화할 수 없습니다.")
        home_return = conn.execute(
            """
            SELECT id FROM schedules
            WHERE home_request_id IS NOT NULL
              AND home_timeout_ack_at IS NULL
            LIMIT 1
            """
        ).fetchone()
        if home_return:
            raise ActiveDoseError("기구가 (0,0)으로 복귀할 때까지 기다려 주세요.")
        empty_slot_schedule = conn.execute(
            """
            SELECT id FROM schedules
            WHERE status = 'FAILED'
              AND error_code = 'EMPTY_BLISTER_SLOT'
              AND acknowledged_at IS NULL
            ORDER BY id LIMIT 1
            """
        ).fetchone()
        conn.execute(
            """
            UPDATE device_state
            SET current_x = 0, current_y = 0,
                blister_exhausted = 0, coordinate_path_version = 2,
                updated_at = ?
            WHERE singleton = 1
            """,
            (event_time,),
        )
        if empty_slot_schedule is not None:
            conn.execute(
                """
                UPDATE schedules
                SET x_coordinate = 0, y_coordinate = 0
                WHERE id = ?
                """,
                (empty_slot_schedule["id"],),
            )
        _insert_event(
            conn,
            (
                empty_slot_schedule["id"]
                if empty_slot_schedule is not None
                else None
            ),
            "BLISTER_RESET",
            event_time,
        )
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def _reset_state_dict(row):
    result = dict(row)
    if result["request_id"] is None:
        result["state"] = "IDLE"
    elif result["completed_at"] is not None:
        result["state"] = "COMPLETED"
    elif result["error_code"] is not None:
        result["state"] = "ERROR"
    else:
        result["state"] = "PENDING"
    return result


def get_system_reset_state():
    conn = connect_db()
    try:
        row = conn.execute(
            "SELECT request_id, requested_at, sent_at, acknowledged_at, "
            "completed_at, error_code "
            "FROM system_reset_state WHERE singleton = 1"
        ).fetchone()
        return _reset_state_dict(row)
    finally:
        conn.close()


def get_pending_system_reset():
    state = get_system_reset_state()
    return state if state["state"] == "PENDING" else None


def get_blocking_system_reset():
    state = get_system_reset_state()
    return state if state["state"] in {"PENDING", "ERROR"} else None


def prepare_system_reset(event_time):
    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        row = conn.execute(
            "SELECT request_id, requested_at, sent_at, acknowledged_at, "
            "completed_at, error_code "
            "FROM system_reset_state WHERE singleton = 1"
        ).fetchone()
        state = _reset_state_dict(row)
        if state["state"] == "PENDING":
            conn.commit()
            return state

        request_id = allocate_request_id(conn)
        conn.execute(
            """
            UPDATE system_reset_state
            SET request_id = ?, requested_at = ?, sent_at = NULL,
                acknowledged_at = NULL, completed_at = NULL,
                error_code = NULL
            WHERE singleton = 1
            """,
            (request_id, event_time),
        )
        conn.commit()
        return get_system_reset_state()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def record_system_reset_sent(request_id, event_time):
    conn = connect_db()
    try:
        cursor = conn.execute(
            """
            UPDATE system_reset_state
            SET sent_at = COALESCE(sent_at, ?)
            WHERE singleton = 1 AND request_id = ?
              AND completed_at IS NULL AND error_code IS NULL
            """,
            (event_time, request_id),
        )
        conn.commit()
        return cursor.rowcount > 0
    finally:
        conn.close()


def mark_system_reset_error(request_id, event_time, error_code):
    conn = connect_db()
    try:
        cursor = conn.execute(
            """
            UPDATE system_reset_state
            SET error_code = COALESCE(error_code, ?)
            WHERE singleton = 1 AND request_id = ?
              AND completed_at IS NULL
            """,
            (error_code, request_id),
        )
        conn.commit()
        return cursor.rowcount > 0
    finally:
        conn.close()


def complete_system_reset(request_id, event_time):
    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        row = conn.execute(
            """
            SELECT request_id
            FROM system_reset_state
            WHERE singleton = 1 AND request_id = ?
              AND completed_at IS NULL AND error_code IS NULL
            """,
            (request_id,),
        ).fetchone()
        if row is None:
            conn.rollback()
            return False

        conn.execute("DELETE FROM schedules")
        conn.execute(
            "DELETE FROM sqlite_sequence "
            "WHERE name IN ('schedules', 'event_log')"
        )
        conn.execute("DELETE FROM event_log")
        conn.execute(
            """
            UPDATE device_state
            SET current_x = 0, current_y = 0,
                blister_exhausted = 0, coordinate_path_version = 2,
                updated_at = ?
            WHERE singleton = 1
            """,
            (event_time,),
        )
        conn.execute(
            """
            UPDATE system_reset_state
            SET acknowledged_at = ?, completed_at = ?, error_code = NULL
            WHERE singleton = 1 AND request_id = ?
            """,
            (event_time, event_time, request_id),
        )
        conn.commit()
        return True
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def reset_schedules_and_position(event_time):
    conn = connect_db()
    try:
        conn.execute("BEGIN IMMEDIATE")
        conn.execute("DELETE FROM schedules")
        conn.execute(
            "DELETE FROM sqlite_sequence WHERE name IN ('schedules', 'event_log')"
        )
        conn.execute("DELETE FROM event_log")
        conn.execute(
            """
            UPDATE device_state
            SET current_x = 0, current_y = 0,
                blister_exhausted = 0, coordinate_path_version = 2,
                updated_at = ?
            WHERE singleton = 1
            """,
            (event_time,),
        )
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def get_device_settings():
    conn = connect_db()
    try:
        row = conn.execute(
            """
            SELECT voice_repeat, volume_step, updated_at
            FROM device_settings WHERE singleton = 1
            """
        ).fetchone()
        return dict(row)
    finally:
        conn.close()


def update_device_settings(voice_repeat, volume_step, event_time):
    voice_repeat = int(voice_repeat)
    if voice_repeat < MIN_VOICE_REPEAT or voice_repeat > MAX_VOICE_REPEAT:
        raise ValueError("voice_repeat must be 0-10")
    volume_step = int(volume_step)
    if volume_step < MIN_VOLUME_STEP or volume_step > MAX_VOLUME_STEP:
        raise ValueError("volume_step must be 0-10")
    conn = connect_db()
    try:
        conn.execute(
            """
            UPDATE device_settings
            SET voice_repeat = ?, volume_step = ?, updated_at = ?
            WHERE singleton = 1
            """,
            (voice_repeat, volume_step, event_time),
        )
        _insert_event(
            conn,
            None,
            "SETTINGS_UPDATED",
            event_time,
            detail=f"voice_repeat={voice_repeat},volume_step={volume_step}",
        )
        conn.commit()
    finally:
        conn.close()


def get_event_log(limit=100):
    conn = connect_db()
    try:
        rows = conn.execute(
            """
            SELECT id, schedule_id, event_type, request_id, detail, created_at
            FROM event_log ORDER BY id DESC LIMIT ?
            """,
            (limit,),
        ).fetchall()
        return [dict(row) for row in rows]
    finally:
        conn.close()


def _insert_event(
    conn,
    schedule_id,
    event_type,
    created_at,
    request_id=None,
    detail=None,
):
    conn.execute(
        """
        INSERT INTO event_log (
            schedule_id, event_type, request_id, detail, created_at
        )
        VALUES (?, ?, ?, ?, ?)
        """,
        (schedule_id, event_type, request_id, detail, created_at),
    )


def get_system_time_state():
    conn = connect_db()
    try:
        row = conn.execute(
            """
            SELECT boot_id, configured_at, source_timezone
            FROM system_time_state WHERE singleton = 1
            """
        ).fetchone()
        return dict(row) if row else None
    finally:
        conn.close()


def mark_system_time_configured(boot_id, configured_at, source_timezone):
    conn = connect_db()
    try:
        conn.execute(
            """
            UPDATE system_time_state
            SET boot_id = ?, configured_at = ?, source_timezone = ?
            WHERE singleton = 1
            """,
            (boot_id, configured_at, source_timezone),
        )
        conn.commit()
    finally:
        conn.close()


# Compatibility wrappers retained for older callers during migration.
def complete_schedule_success(schedule_id, event_time):
    schedule = get_schedule(schedule_id)
    if not schedule:
        return None
    if schedule["status"] == "READY_TO_DISPENSE":
        schedule = prepare_dispense(schedule_id, event_time)
    return complete_dispensed(
        schedule_id,
        schedule["dispense_request_id"],
        event_time,
    )


def update_status(schedule_id, status, event_time=None):
    status_map = {
        "WAITING": "SCHEDULED",
        "ALLOWED": "READY_TO_DISPENSE",
    }
    status = status_map.get(status, status)
    if status == "MISSED":
        return int(mark_missed(
            schedule_id,
            event_time or datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        ))
    raise ValueError(f"Unsupported compatibility status: {status}")
