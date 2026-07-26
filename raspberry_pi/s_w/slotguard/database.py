import os
import re
import sqlite3
from pathlib import Path


DEFAULT_DB_PATH = Path(__file__).resolve().parent / "slotguard.db"
DB_PATH = Path(os.environ.get("SLOTGUARD_DB_PATH", DEFAULT_DB_PATH))
MAX_ALLOWED_SECONDS = 999999

SCHEDULE_COLUMNS = {
    "id",
    "scheduled_at",
    "allowed_seconds",
    "status",
    "x_coordinate",
    "y_coordinate",
    "dispensed_at",
    "synced",
    "command_sent_at",
    "ack_received_at",
}


def connect_db():
    conn = sqlite3.connect(DB_PATH, timeout=5)
    conn.row_factory = sqlite3.Row
    return conn


def _create_schedules_table(conn):
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS schedules (
            id               INTEGER PRIMARY KEY AUTOINCREMENT,
            scheduled_at     TEXT NOT NULL,
            allowed_seconds  INTEGER NOT NULL DEFAULT 3600
                             CHECK(allowed_seconds BETWEEN 1 AND 999999),
            status           TEXT NOT NULL DEFAULT 'WAITING'
                             CHECK(status IN (
                                 'WAITING',
                                 'ALLOWED',
                                 'DISPENSED',
                                 'MISSED'
                             )),
            x_coordinate     INTEGER
                             CHECK(x_coordinate BETWEEN 0 AND 1),
            y_coordinate     INTEGER
                             CHECK(y_coordinate BETWEEN 0 AND 4),
            dispensed_at     TEXT,
            synced           INTEGER NOT NULL DEFAULT 0
                             CHECK(synced IN (0, 1)),
            command_sent_at  TEXT,
            ack_received_at  TEXT,

            CHECK(
                (x_coordinate IS NULL AND y_coordinate IS NULL)
                OR
                (x_coordinate IS NOT NULL AND y_coordinate IS NOT NULL)
            )
        )
        """
    )


def _migrate_legacy_schedules(conn):
    legacy_rows = conn.execute(
        """
        SELECT
            scheduled_at,
            status,
            dispensed_at,
            synced
        FROM schedules
        ORDER BY id
        """
    ).fetchall()

    conn.execute("ALTER TABLE schedules RENAME TO schedules_legacy")
    _create_schedules_table(conn)

    conn.executemany(
        """
        INSERT INTO schedules (
            scheduled_at,
            allowed_seconds,
            status,
            dispensed_at,
            synced
        )
        VALUES (?, 3600, ?, ?, ?)
        """,
        [
            (
                row["scheduled_at"],
                (
                    "WAITING"
                    if (
                        row["status"] == "ALLOWED"
                        and not row["synced"]
                    )
                    else row["status"]
                ),
                row["dispensed_at"],
                row["synced"],
            )
            for row in legacy_rows
        ],
    )

    conn.execute("DROP TABLE schedules_legacy")


def _get_allowed_seconds_limit(conn):
    row = conn.execute(
        """
        SELECT sql
        FROM sqlite_master
        WHERE type = 'table'
          AND name = 'schedules'
        """
    ).fetchone()

    if row is None or not row["sql"]:
        return None

    match = re.search(
        r"allowed_seconds\s+BETWEEN\s+1\s+AND\s+(\d+)",
        row["sql"],
        flags=re.IGNORECASE,
    )

    return int(match.group(1)) if match else None


def _expand_allowed_seconds_limit(conn):
    conn.execute("ALTER TABLE schedules RENAME TO schedules_previous")
    _create_schedules_table(conn)
    conn.execute(
        """
        INSERT INTO schedules (
            id,
            scheduled_at,
            allowed_seconds,
            status,
            x_coordinate,
            y_coordinate,
            dispensed_at,
            synced,
            command_sent_at,
            ack_received_at
        )
        SELECT
            id,
            scheduled_at,
            allowed_seconds,
            status,
            x_coordinate,
            y_coordinate,
            dispensed_at,
            synced,
            command_sent_at,
            ack_received_at
        FROM schedules_previous
        ORDER BY id
        """
    )
    conn.execute("DROP TABLE schedules_previous")


def init_db():
    conn = connect_db()

    try:
        table_exists = conn.execute(
            """
            SELECT 1
            FROM sqlite_master
            WHERE type = 'table'
              AND name = 'schedules'
            """
        ).fetchone()

        if table_exists:
            columns = {
                row["name"]
                for row in conn.execute(
                    "PRAGMA table_info(schedules)"
                ).fetchall()
            }

            if not SCHEDULE_COLUMNS.issubset(columns):
                _migrate_legacy_schedules(conn)
            elif _get_allowed_seconds_limit(conn) != MAX_ALLOWED_SECONDS:
                _expand_allowed_seconds_limit(conn)
        else:
            _create_schedules_table(conn)

        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS device_state (
                singleton      INTEGER PRIMARY KEY
                               CHECK(singleton = 1),
                current_x      INTEGER NOT NULL DEFAULT 0
                               CHECK(current_x BETWEEN 0 AND 1),
                current_y      INTEGER NOT NULL DEFAULT 0
                               CHECK(current_y BETWEEN 0 AND 4),
                updated_at     TEXT
            )
            """
        )

        conn.execute(
            """
            INSERT OR IGNORE INTO device_state (
                singleton,
                current_x,
                current_y
            )
            VALUES (1, 0, 0)
            """
        )

        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS system_time_state (
                singleton        INTEGER PRIMARY KEY
                                 CHECK(singleton = 1),
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
            CREATE INDEX IF NOT EXISTS idx_schedules_due
            ON schedules(status, scheduled_at, id)
            """
        )

        conn.execute("PRAGMA user_version = 2")
        conn.commit()

    except Exception:
        conn.rollback()
        raise

    finally:
        conn.close()


def create_schedule(scheduled_at, allowed_seconds=3600):
    if allowed_seconds < 1 or allowed_seconds > MAX_ALLOWED_SECONDS:
        raise ValueError(
            "allowed_seconds must be between "
            f"1 and {MAX_ALLOWED_SECONDS}"
        )

    conn = connect_db()

    try:
        cursor = conn.execute(
            """
            INSERT INTO schedules (
                scheduled_at,
                allowed_seconds
            )
            VALUES (?, ?)
            """,
            (scheduled_at, allowed_seconds),
        )

        conn.commit()
        return cursor.lastrowid

    finally:
        conn.close()


def get_schedules():
    conn = connect_db()

    try:
        rows = conn.execute(
            """
            SELECT
                id,
                scheduled_at,
                allowed_seconds,
                status,
                x_coordinate,
                y_coordinate,
                dispensed_at,
                synced,
                command_sent_at,
                ack_received_at
            FROM schedules
            ORDER BY scheduled_at, id
            """
        ).fetchall()

        return [dict(row) for row in rows]

    finally:
        conn.close()


def get_schedule(schedule_id):
    conn = connect_db()

    try:
        row = conn.execute(
            """
            SELECT
                id,
                scheduled_at,
                allowed_seconds,
                status,
                x_coordinate,
                y_coordinate,
                dispensed_at,
                synced,
                command_sent_at,
                ack_received_at
            FROM schedules
            WHERE id = ?
            """,
            (schedule_id,),
        ).fetchone()

        return dict(row) if row else None

    finally:
        conn.close()


def delete_schedule(schedule_id):
    conn = connect_db()

    try:
        cursor = conn.execute(
            "DELETE FROM schedules WHERE id = ?",
            (schedule_id,),
        )
        conn.commit()

        return cursor.rowcount > 0

    finally:
        conn.close()


def get_active_schedule():
    conn = connect_db()

    try:
        row = conn.execute(
            """
            SELECT
                id,
                scheduled_at,
                allowed_seconds,
                status,
                x_coordinate,
                y_coordinate,
                dispensed_at,
                synced,
                command_sent_at,
                ack_received_at
            FROM schedules
            WHERE status IN ('WAITING', 'ALLOWED')
              AND x_coordinate IS NOT NULL
              AND y_coordinate IS NOT NULL
            ORDER BY scheduled_at, id
            LIMIT 1
            """
        ).fetchone()

        return dict(row) if row else None

    finally:
        conn.close()


def get_due_schedule(current_time):
    conn = connect_db()

    try:
        row = conn.execute(
            """
            SELECT
                id,
                scheduled_at,
                allowed_seconds,
                status,
                x_coordinate,
                y_coordinate,
                dispensed_at,
                synced,
                command_sent_at,
                ack_received_at
            FROM schedules
            WHERE status = 'WAITING'
              AND x_coordinate IS NULL
              AND scheduled_at <= ?
            ORDER BY scheduled_at, id
            LIMIT 1
            """,
            (current_time,),
        ).fetchone()

        return dict(row) if row else None

    finally:
        conn.close()


def expire_unstarted_schedules(current_time):
    conn = connect_db()

    try:
        cursor = conn.execute(
            """
            UPDATE schedules
            SET status = 'MISSED'
            WHERE status = 'WAITING'
              AND command_sent_at IS NULL
              AND datetime(
                    scheduled_at,
                    '+' || allowed_seconds || ' seconds'
                  ) <= datetime(?)
            """,
            (current_time,),
        )

        conn.commit()
        return cursor.rowcount

    finally:
        conn.close()


def assign_coordinate(schedule_id, x_coordinate, y_coordinate):
    conn = connect_db()

    try:
        conn.execute(
            """
            UPDATE schedules
            SET x_coordinate = ?,
                y_coordinate = ?
            WHERE id = ?
              AND status = 'WAITING'
              AND x_coordinate IS NULL
              AND y_coordinate IS NULL
            """,
            (x_coordinate, y_coordinate, schedule_id),
        )

        conn.commit()
        row = conn.execute(
            """
            SELECT
                id,
                scheduled_at,
                allowed_seconds,
                status,
                x_coordinate,
                y_coordinate,
                dispensed_at,
                synced,
                command_sent_at,
                ack_received_at
            FROM schedules
            WHERE id = ?
            """,
            (schedule_id,),
        ).fetchone()

        return dict(row) if row else None

    finally:
        conn.close()


def record_command_sent(schedule_id, event_time):
    conn = connect_db()

    try:
        conn.execute(
            """
            UPDATE schedules
            SET command_sent_at = COALESCE(command_sent_at, ?)
            WHERE id = ?
              AND status = 'WAITING'
            """,
            (event_time, schedule_id),
        )

        conn.commit()

    finally:
        conn.close()


def mark_synced(schedule_id, event_time):
    conn = connect_db()

    try:
        conn.execute(
            """
            UPDATE schedules
            SET synced = 1,
                status = 'ALLOWED',
                ack_received_at = COALESCE(ack_received_at, ?)
            WHERE id = ?
              AND status = 'WAITING'
            """,
            (event_time, schedule_id),
        )

        conn.commit()

    finally:
        conn.close()


def update_status(schedule_id, status, event_time=None):
    if status not in {"WAITING", "ALLOWED", "DISPENSED", "MISSED"}:
        raise ValueError(f"Unsupported schedule status: {status}")

    conn = connect_db()

    try:
        if status == "DISPENSED":
            cursor = conn.execute(
                """
                UPDATE schedules
                SET status = ?,
                    dispensed_at = ?
                WHERE id = ?
                """,
                (status, event_time, schedule_id),
            )
        else:
            cursor = conn.execute(
                """
                UPDATE schedules
                SET status = ?
                WHERE id = ?
                """,
                (status, schedule_id),
            )

        conn.commit()
        return cursor.rowcount

    finally:
        conn.close()


def get_current_coordinate():
    conn = connect_db()

    try:
        row = conn.execute(
            """
            SELECT current_x, current_y
            FROM device_state
            WHERE singleton = 1
            """
        ).fetchone()

        return row["current_x"], row["current_y"]

    finally:
        conn.close()


def complete_schedule_success(schedule_id, event_time):
    conn = connect_db()

    try:
        conn.execute("BEGIN IMMEDIATE")
        cursor = conn.execute(
            """
            UPDATE schedules
            SET status = 'DISPENSED',
                dispensed_at = ?
            WHERE id = ?
              AND status IN ('WAITING', 'ALLOWED')
            """,
            (event_time, schedule_id),
        )

        if cursor.rowcount == 0:
            conn.rollback()
            return None

        row = conn.execute(
            """
            SELECT current_x, current_y
            FROM device_state
            WHERE singleton = 1
            """
        ).fetchone()

        current_x = row["current_x"]
        current_y = row["current_y"]
        blister_exhausted = current_x == 1 and current_y == 4

        if blister_exhausted:
            next_x, next_y = 0, 0
        elif current_y < 4:
            next_x, next_y = current_x, current_y + 1
        else:
            next_x, next_y = 1, 0

        conn.execute(
            """
            UPDATE device_state
            SET current_x = ?,
                current_y = ?,
                updated_at = ?
            WHERE singleton = 1
            """,
            (next_x, next_y, event_time),
        )

        conn.commit()

        return {
            "previous": (current_x, current_y),
            "current": (next_x, next_y),
            "blister_exhausted": blister_exhausted,
        }

    except Exception:
        conn.rollback()
        raise

    finally:
        conn.close()


def advance_coordinate(event_time):
    conn = connect_db()

    try:
        conn.execute("BEGIN IMMEDIATE")

        row = conn.execute(
            """
            SELECT current_x, current_y
            FROM device_state
            WHERE singleton = 1
            """
        ).fetchone()

        current_x = row["current_x"]
        current_y = row["current_y"]
        blister_exhausted = current_x == 1 and current_y == 4

        if blister_exhausted:
            next_x, next_y = 0, 0
        elif current_y < 4:
            next_x, next_y = current_x, current_y + 1
        else:
            next_x, next_y = 1, 0

        conn.execute(
            """
            UPDATE device_state
            SET current_x = ?,
                current_y = ?,
                updated_at = ?
            WHERE singleton = 1
            """,
            (next_x, next_y, event_time),
        )

        conn.commit()

        return {
            "previous": (current_x, current_y),
            "current": (next_x, next_y),
            "blister_exhausted": blister_exhausted,
        }

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
            "DELETE FROM sqlite_sequence WHERE name = 'schedules'"
        )
        conn.execute(
            """
            UPDATE device_state
            SET current_x = 0,
                current_y = 0,
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


def get_system_time_state():
    conn = connect_db()

    try:
        row = conn.execute(
            """
            SELECT
                boot_id,
                configured_at,
                source_timezone
            FROM system_time_state
            WHERE singleton = 1
            """
        ).fetchone()

        return dict(row) if row else None

    finally:
        conn.close()


def mark_system_time_configured(
    boot_id,
    configured_at,
    source_timezone,
):
    conn = connect_db()

    try:
        conn.execute(
            """
            UPDATE system_time_state
            SET boot_id = ?,
                configured_at = ?,
                source_timezone = ?
            WHERE singleton = 1
            """,
            (boot_id, configured_at, source_timezone),
        )
        conn.commit()

    finally:
        conn.close()
