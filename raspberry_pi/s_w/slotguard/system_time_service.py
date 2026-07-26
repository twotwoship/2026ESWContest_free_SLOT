import os
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from database import (
    get_system_time_state,
    mark_system_time_configured,
)


BOOT_ID_PATH = Path(
    os.environ.get(
        "SLOTGUARD_BOOT_ID_PATH",
        "/proc/sys/kernel/random/boot_id",
    )
)
TIME_HELPER_PATH = Path(
    os.environ.get(
        "SLOTGUARD_TIME_HELPER",
        "/usr/local/sbin/slotguard-set-time",
    )
)

DEVICE_TIMEZONE_NAME = "Asia/Seoul"
DEVICE_TIMEZONE = ZoneInfo(DEVICE_TIMEZONE_NAME)
MIN_TIMESTAMP_SECONDS = int(
    datetime(2024, 1, 1, tzinfo=timezone.utc).timestamp()
)
MAX_TIMESTAMP_SECONDS = int(
    datetime(2100, 1, 1, tzinfo=timezone.utc).timestamp()
)


class SystemTimeError(Exception):
    def __init__(self, message, status_code=400):
        super().__init__(message)
        self.status_code = status_code


def get_current_boot_id():
    try:
        boot_id = BOOT_ID_PATH.read_text(encoding="ascii").strip()
    except OSError:
        return None

    return boot_id or None


def is_system_time_configured():
    boot_id = get_current_boot_id()
    state = get_system_time_state()

    return bool(
        boot_id
        and state
        and state["configured_at"]
        and state["boot_id"] == boot_id
    )


def get_system_time_status():
    state = get_system_time_state()

    return {
        "configured": is_system_time_configured(),
        "configured_at": (
            state["configured_at"] if state else None
        ),
        "source_timezone": (
            state["source_timezone"] if state else None
        ),
        "device_time": datetime.now().strftime(
            "%Y-%m-%d %H:%M:%S"
        ),
        "device_timezone": DEVICE_TIMEZONE_NAME,
    }


def _validate_smartphone_time(timestamp_ms, source_timezone):
    if isinstance(timestamp_ms, bool) or not isinstance(timestamp_ms, int):
        raise SystemTimeError("스마트폰 시간이 올바르지 않습니다.")

    timestamp_seconds = int(round(timestamp_ms / 1000))

    if not (
        MIN_TIMESTAMP_SECONDS
        <= timestamp_seconds
        < MAX_TIMESTAMP_SECONDS
    ):
        raise SystemTimeError(
            "설정 가능한 날짜 범위는 2024년부터 2099년까지입니다."
        )

    if (
        not isinstance(source_timezone, str)
        or not source_timezone
        or len(source_timezone) > 64
    ):
        raise SystemTimeError("스마트폰 시간대가 올바르지 않습니다.")

    try:
        ZoneInfo(source_timezone)
    except ZoneInfoNotFoundError as error:
        raise SystemTimeError(
            "지원하지 않는 스마트폰 시간대입니다."
        ) from error

    return timestamp_seconds


def _run_time_helper(timestamp_seconds):
    if not TIME_HELPER_PATH.is_file():
        raise SystemTimeError(
            "시스템 시간 도우미가 설치되지 않았습니다.",
            status_code=503,
        )

    if os.geteuid() == 0:
        command = [
            str(TIME_HELPER_PATH),
            str(timestamp_seconds),
        ]
    else:
        command = [
            "/usr/bin/sudo",
            "-n",
            str(TIME_HELPER_PATH),
            str(timestamp_seconds),
        ]

    try:
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise SystemTimeError(
            "Pi 시스템 시간을 변경하지 못했습니다.",
            status_code=503,
        ) from error

    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise SystemTimeError(
            detail or "Pi 시스템 시간 변경 권한이 없습니다.",
            status_code=503,
        )


def set_system_time_from_smartphone(
    timestamp_ms,
    source_timezone,
):
    timestamp_seconds = _validate_smartphone_time(
        timestamp_ms,
        source_timezone,
    )
    _run_time_helper(timestamp_seconds)

    boot_id = get_current_boot_id()

    if not boot_id:
        raise SystemTimeError(
            "Pi 부팅 식별자를 확인할 수 없습니다.",
            status_code=503,
        )

    configured_time = datetime.fromtimestamp(
        timestamp_seconds,
        DEVICE_TIMEZONE,
    ).strftime("%Y-%m-%d %H:%M:%S")

    mark_system_time_configured(
        boot_id=boot_id,
        configured_at=configured_time,
        source_timezone=source_timezone,
    )

    return {
        "configured": True,
        "device_time": configured_time,
        "device_timezone": DEVICE_TIMEZONE_NAME,
    }
