import json
import os
import secrets
import socket
import sqlite3
import subprocess
import threading
import time
from datetime import datetime, timedelta
from functools import wraps
from pathlib import Path

from flask import (
    Flask,
    jsonify,
    redirect,
    render_template,
    render_template_string,
    request,
    session,
    url_for,
)
from werkzeug.security import check_password_hash, generate_password_hash

from database import (
    ACK_REQUIRED_STATUSES,
    ActiveDoseError,
    DuplicateScheduleError,
    MAX_ALLOWED_SECONDS,
    MAX_MEDICINE_NAME_LENGTH,
    acknowledge_result,
    complete_empty_blister_manual,
    complete_manual,
    create_schedule,
    delete_schedule,
    get_active_schedule,
    get_current_coordinate,
    get_device_settings,
    get_device_state,
    get_latest_result,
    get_next_schedule,
    get_recent_records,
    get_schedule,
    get_schedules,
    get_unacknowledged_result,
    get_used_coordinates,
    init_db,
    reset_blister,
    reset_schedules_and_position,
    update_device_settings,
)
from system_time_service import (
    SystemTimeError,
    get_system_time_status,
    is_system_time_configured,
    set_system_time_from_smartphone,
)
from uart_service import UartService


BASE_DIR = Path(__file__).resolve().parent
AUTH_FILE = BASE_DIR / "slotguard_auth.json"
POWER_HELPER_PATH = Path(
    os.environ.get(
        "SLOTGUARD_POWER_HELPER",
        "/usr/local/sbin/slotguard-power",
    )
)
SESSION_IDLE_TIMEOUT_SECONDS = 30 * 60
APP_VERSION = "0.3.0"


def load_auth_config():
    if AUTH_FILE.is_file():
        return json.loads(AUTH_FILE.read_text(encoding="utf-8"))

    return {
        "secret_key": secrets.token_hex(32),
        "admin_id": None,
        "password_hash": None,
    }


def save_auth_config():
    AUTH_FILE.write_text(
        json.dumps(AUTH_CONFIG, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    AUTH_FILE.chmod(0o600)


def auth_configured():
    return bool(
        AUTH_CONFIG["admin_id"]
        and AUTH_CONFIG["password_hash"]
    )


AUTH_CONFIG = load_auth_config()

app = Flask(__name__)
app.config["SECRET_KEY"] = AUTH_CONFIG["secret_key"]
app.config["SESSION_COOKIE_HTTPONLY"] = True
app.config["SESSION_COOKIE_SAMESITE"] = "Lax"
app.config["PERMANENT_SESSION_LIFETIME"] = timedelta(
    seconds=SESSION_IDLE_TIMEOUT_SECONDS
)
app.config["SESSION_REFRESH_EACH_REQUEST"] = True

init_db()

MEDICINE_VOICE_FILE = (
    BASE_DIR
    / "audio"
    / "medicine_time.mp3"
)
BLISTER_EMPTY_VOICE_FILE = (
    BASE_DIR
    / "audio"
    / "blister_empty.mp3"
)

AUTH_PAGE_TEMPLATE = """
<!DOCTYPE html>
<html lang="ko">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>SLOT-GUARD {{ page_title }}</title>
    <style>
        * {
            box-sizing: border-box;
        }

        body {
            margin: 0;
            font-family: sans-serif;
            background: #f3f4f6;
        }

        main {
            width: min(360px, calc(100% - 40px));
            margin: 80px auto;
            padding: 24px;
            background: white;
            border-radius: 12px;
            box-shadow: 0 4px 16px rgba(0, 0, 0, 0.12);
        }

        h1 {
            margin-top: 0;
        }

        input,
        button {
            width: 100%;
            margin-top: 12px;
            padding: 12px;
            font-size: 16px;
        }

        button {
            color: white;
            background: #2563eb;
            border: 0;
            border-radius: 6px;
            cursor: pointer;
        }

        .error {
            color: #dc2626;
        }
    </style>
</head>
<body>
    <main>
        <h1>SLOT-GUARD</h1>
        <p>{{ description }}</p>

        {% if error %}
            <p class="error">{{ error }}</p>
        {% endif %}

        <form method="post">
            <input
                type="text"
                name="username"
                placeholder="아이디"
                autocomplete="username"
                required
                autofocus
            >

            <input
                type="password"
                name="password"
                placeholder="비밀번호"
                autocomplete="{{ password_autocomplete }}"
                required
            >

            {% if setup %}
                <input
                    type="password"
                    name="password_confirm"
                    placeholder="비밀번호 확인"
                    autocomplete="new-password"
                    required
                >
            {% endif %}

            <button type="submit">{{ button_text }}</button>
        </form>
    </main>
</body>
</html>
"""


VOICE_GAIN_BY_STEP = (
    "0.0",
    "0.25",
    "0.40",
    "0.55",
    "0.75",
    "1.00",
    "1.10",
    "1.20",
    "1.30",
    "1.40",
    "1.50",
)


def voice_command(voice_file, volume_step):
    return [
        "/usr/bin/cvlc",
        "--intf", "dummy",
        "--play-and-exit",
        "--no-video",
        "--gain", VOICE_GAIN_BY_STEP[int(volume_step)],
        "-A", "alsa",
        "--alsa-audio-device", "sysdefault:CARD=Headphones",
        str(voice_file),
    ]


def play_voice(voice_file, description, volume_step=5):
    if not voice_file.is_file():
        print(f"[VOICE] 음성 파일을 찾을 수 없습니다: {voice_file}")
        return

    if int(volume_step) == 0:
        return

    result = subprocess.run(
        voice_command(voice_file, volume_step),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )

    if result.returncode != 0:
        print(
            f"[VOICE] {description} 재생 실패: "
            f"returncode={result.returncode}"
        )


def play_voice_async(voice_file, description, volume_step=5):
    threading.Thread(
        target=play_voice,
        args=(voice_file, description, volume_step),
        daemon=True,
    ).start()


class VoiceAlertManager:
    def __init__(self):
        self._lock = threading.Lock()
        self._stop_event = None
        self._process = None

    def start(self):
        self.stop()
        settings = get_device_settings()
        if settings["voice_repeat"] == 0 or settings["volume_step"] == 0:
            return

        stop_event = threading.Event()
        with self._lock:
            self._stop_event = stop_event

        threading.Thread(
            target=self._run,
            args=(
                stop_event,
                settings["voice_repeat"],
                settings["volume_step"],
            ),
            name="slotguard-voice-alert",
            daemon=True,
        ).start()

    def test_once(self, volume_step):
        self.stop()
        if int(volume_step) == 0:
            return False

        stop_event = threading.Event()
        with self._lock:
            self._stop_event = stop_event

        threading.Thread(
            target=self._run,
            args=(stop_event, 1, volume_step),
            name="slotguard-volume-test",
            daemon=True,
        ).start()
        return True

    def stop(self):
        with self._lock:
            stop_event = self._stop_event
            process = self._process
            self._stop_event = None
            self._process = None
        if stop_event is not None:
            stop_event.set()
        if process is not None and process.poll() is None:
            process.terminate()

    def _run(self, stop_event, repeat_count, volume_step):
        if not MEDICINE_VOICE_FILE.is_file():
            print(
                f"[VOICE] 음성 파일을 찾을 수 없습니다: "
                f"{MEDICINE_VOICE_FILE}"
            )
            return

        for repeat_index in range(repeat_count):
            if stop_event.is_set():
                break
            process = subprocess.Popen(
                voice_command(MEDICINE_VOICE_FILE, volume_step),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            with self._lock:
                if self._stop_event is stop_event:
                    self._process = process
            process.wait()
            with self._lock:
                if self._process is process:
                    self._process = None

            if repeat_index < repeat_count - 1 and stop_event.wait(5):
                break


def play_blister_empty_voice():
    settings = get_device_settings()
    if settings["voice_repeat"] == 0:
        return
    play_voice_async(
        BLISTER_EMPTY_VOICE_FILE,
        "블리스터 소진 안내",
        settings["volume_step"],
    )


voice_alert_manager = VoiceAlertManager()


uart_service = UartService(
    on_schedule_started=voice_alert_manager.start,
    on_alert_stop=voice_alert_manager.stop,
    on_blister_exhausted=play_blister_empty_voice,
    is_time_ready=is_system_time_configured,
)


def current_timestamp():
    return int(time.time())


def start_admin_session(username):
    session.clear()
    session.permanent = True
    session["logged_in"] = True
    session["username"] = username
    session["last_activity"] = current_timestamp()


def login_required(view):
    @wraps(view)
    def wrapped_view(*args, **kwargs):
        if not auth_configured():
            if request.path.startswith("/api/"):
                return jsonify({"error": "SETUP_REQUIRED"}), 503

            return redirect(url_for("setup"))

        if not session.get("logged_in"):
            if request.path.startswith("/api/"):
                return jsonify({"error": "LOGIN_REQUIRED"}), 401

            return redirect(url_for("login"))

        current_time = current_timestamp()
        last_activity = session.get("last_activity")

        if last_activity is not None:
            try:
                session_expired = (
                    current_time - int(last_activity)
                    >= SESSION_IDLE_TIMEOUT_SECONDS
                )
            except (TypeError, ValueError):
                session_expired = True

            if session_expired:
                session.clear()

                if request.path.startswith("/api/"):
                    return jsonify({
                        "error": "SESSION_EXPIRED",
                    }), 401

                return redirect(url_for("login", expired=1))

        session.permanent = True
        session["last_activity"] = current_time

        return view(*args, **kwargs)

    return wrapped_view


@app.route("/setup", methods=["GET", "POST"])
def setup():
    if auth_configured():
        return redirect(url_for("login"))

    error = None

    if request.method == "POST":
        username = request.form.get("username", "").strip()
        password = request.form.get("password", "")
        password_confirm = request.form.get(
            "password_confirm",
            "",
        )

        if not username:
            error = "아이디를 입력하십시오."
        elif len(password) < 8:
            error = "비밀번호는 8자 이상이어야 합니다."
        elif password != password_confirm:
            error = "비밀번호 확인이 일치하지 않습니다."
        else:
            AUTH_CONFIG["admin_id"] = username
            AUTH_CONFIG["password_hash"] = generate_password_hash(
                password,
            )
            save_auth_config()

            start_admin_session(username)

            return redirect(url_for("dashboard"))

    return render_template_string(
        AUTH_PAGE_TEMPLATE,
        page_title="초기 설정",
        description="처음 사용할 관리자 계정을 만드십시오.",
        error=error,
        setup=True,
        password_autocomplete="new-password",
        button_text="관리자 계정 만들기",
    )


@app.route("/login", methods=["GET", "POST"])
def login():
    if not auth_configured():
        return redirect(url_for("setup"))

    error = (
        "30분 동안 활동이 없어 자동으로 로그아웃되었습니다."
        if request.args.get("expired") == "1"
        else None
    )

    if request.method == "POST":
        username = request.form.get("username", "")
        password = request.form.get("password", "")

        if (
            username == AUTH_CONFIG["admin_id"]
            and check_password_hash(
                AUTH_CONFIG["password_hash"],
                password,
            )
        ):
            start_admin_session(username)

            return redirect(url_for("dashboard"))

        error = "아이디 또는 비밀번호가 올바르지 않습니다."

    return render_template_string(
        AUTH_PAGE_TEMPLATE,
        page_title="로그인",
        description="관리자 로그인",
        error=error,
        setup=False,
        password_autocomplete="current-password",
        button_text="로그인",
    )


@app.route("/logout", methods=["GET", "POST"])
def logout():
    session.clear()

    return redirect(url_for("login"))


@app.get("/")
@login_required
def dashboard():
    schedules = get_schedules()
    current_x, current_y = get_current_coordinate()
    uart_status = uart_service.get_status()
    system_time_status = get_system_time_status()

    counts = {
        "total": len(schedules),
        "waiting": 0,
        "allowed": 0,
        "dispensed": 0,
        "missed": 0,
    }

    for schedule in schedules:
        status = schedule["status"]

        if status == "SCHEDULED":
            counts["waiting"] += 1
        elif status in {"MOVING", "READY_TO_DISPENSE", "DISPENSING"}:
            counts["allowed"] += 1
        elif status in {"DISPENSED", "MANUALLY_COMPLETED"}:
            counts["dispensed"] += 1
        elif status in {"FAILED", "MISSED", "COMM_ERROR"}:
            counts["missed"] += 1

    return render_template(
        "dashboard.html",
        counts=counts,
        current_coordinate=f"{current_x}{current_y}",
        uart_status=uart_status["uart"],
        system_time_status=system_time_status,
    )


def parse_allowed_seconds(form):
    hours_text = form.get("allowed_hours", "").strip()
    minutes_text = form.get("allowed_minutes", "").strip()

    if not hours_text and not minutes_text:
        return 3600

    hours = int(hours_text or 0)
    minutes = int(minutes_text or 0)

    if hours < 0 or minutes < 0 or minutes > 59:
        raise ValueError

    allowed_seconds = hours * 3600 + minutes * 60

    if allowed_seconds < 1 or allowed_seconds > MAX_ALLOWED_SECONDS:
        raise ValueError

    return allowed_seconds


@app.route("/schedules", methods=["GET", "POST"])
@login_required
def schedule_page():
    error = None

    if request.method == "POST":
        try:
            scheduled_date = request.form["scheduled_date"]
            scheduled_time = request.form["scheduled_time"]
            medicine_name = request.form["medicine_name"].strip()
            allowed_seconds = parse_allowed_seconds(request.form)

            scheduled_at = datetime.strptime(
                f"{scheduled_date} {scheduled_time}",
                "%Y-%m-%d %H:%M",
            ).strftime("%Y-%m-%d %H:%M")

            create_schedule(
                scheduled_at=scheduled_at,
                medicine_name=medicine_name,
                allowed_seconds=allowed_seconds,
            )

            return redirect(url_for("schedule_page"))

        except DuplicateScheduleError as duplicate_error:
            error = str(duplicate_error)
        except (KeyError, ValueError):
            error = (
                f"약 이름(최대 {MAX_MEDICINE_NAME_LENGTH}자), "
                "날짜·시각과 허용시간을 확인하십시오. "
                "허용시간은 최대 277시간 46분입니다."
            )

    schedules = get_schedules()
    current_x, current_y = get_current_coordinate()

    return render_template(
        "schedules.html",
        schedules=schedules,
        error=error,
        form_values=request.form,
        current_coordinate=f"{current_x}{current_y}",
        max_allowed_hours=MAX_ALLOWED_SECONDS // 3600,
        max_medicine_name_length=MAX_MEDICINE_NAME_LENGTH,
    )


@app.post("/schedules/<int:schedule_id>/delete")
@login_required
def delete_schedule_item(schedule_id):
    if delete_schedule(schedule_id):
        uart_service.cancel_schedule(schedule_id)

    return redirect(url_for("schedule_page"))


@app.post("/schedules/reset")
@login_required
def reset_schedules():
    reset_schedules_and_position(
        datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    )
    uart_service.reset_runtime_state()

    return redirect(url_for("dashboard"))


@app.get("/api/status")
@login_required
def api_status():
    try:
        schedules = get_schedules()

    except sqlite3.Error:
        return jsonify({
            "web": "OK",
            "database": "ERROR",
            "uart": "NOT_CONNECTED",
        }), 500

    current_x, current_y = get_current_coordinate()
    uart_status = uart_service.get_status()
    system_time_status = get_system_time_status()
    response = {
        "web": "OK",
        "database": "OK",
        "uart": uart_status["uart"],
        "uart_port": uart_status["port"],
        "uart_baud_rate": uart_status["baud_rate"],
        "active_schedule_id": uart_status["active_schedule_id"],
        "last_uart_message": uart_status["last_message"],
        "last_uart_error": uart_status["last_error"],
        "current_coordinate": f"{current_x}{current_y}",
        "schedule_count": len(schedules),
        "system_time": system_time_status,
    }

    return jsonify(response)


@app.post("/api/system-time")
@login_required
def api_set_system_time():
    data = request.get_json(silent=True)

    if not isinstance(data, dict):
        return jsonify({
            "error": "INVALID_REQUEST",
            "message": "JSON 요청이 필요합니다.",
        }), 400

    try:
        result = set_system_time_from_smartphone(
            timestamp_ms=data.get("timestamp_ms"),
            source_timezone=data.get("timezone"),
        )
    except SystemTimeError as error:
        return jsonify({
            "error": "TIME_SET_FAILED",
            "message": str(error),
        }), error.status_code

    session.permanent = True
    session["last_activity"] = current_timestamp()

    return jsonify(result)


def display_local_only(view):
    @wraps(view)
    def wrapped_view(*args, **kwargs):
        if request.remote_addr not in {"127.0.0.1", "::1"}:
            return jsonify({
                "error": "LOCAL_DISPLAY_ONLY",
                "message": "장치 LCD에서만 실행할 수 있습니다.",
            }), 403
        return view(*args, **kwargs)

    return wrapped_view


def get_local_ip_address():
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    except OSError:
        return "연결 안 됨"
    try:
        try:
            sock.connect(("192.0.2.1", 9))
            return sock.getsockname()[0]
        except OSError:
            return "연결 안 됨"
    finally:
        sock.close()


def serialize_schedule(schedule):
    if schedule is None:
        return None
    return {
        "id": schedule["id"],
        "medicine_name": schedule["medicine_name"],
        "scheduled_at": schedule["scheduled_at"],
        "allowed_seconds": schedule["allowed_seconds"],
        "status": schedule["status"],
        "x": schedule["x_coordinate"],
        "y": schedule["y_coordinate"],
        "error_code": schedule["error_code"],
        "completed_at": schedule["completed_at"],
    }


def seconds_until_deadline(schedule, now):
    if schedule is None:
        return None
    scheduled_at = datetime.strptime(
        schedule["scheduled_at"],
        "%Y-%m-%d %H:%M",
    )
    deadline = scheduled_at + timedelta(
        seconds=schedule["allowed_seconds"]
    )
    return max(0, int((deadline - now).total_seconds()))


def display_screen_state(now, active, blocking_result, latest_result, state):
    if (
        blocking_result is not None
        and blocking_result["error_code"] == "EMPTY_BLISTER_SLOT"
    ):
        if state["blister_exhausted"]:
            return "BLISTER_EMPTY", blocking_result
        return "EMPTY_BLISTER_CONFIRM", blocking_result

    if blocking_result is not None:
        return blocking_result["status"], blocking_result

    if active is not None:
        return active["status"], active

    if latest_result and latest_result["status"] in {
        "DISPENSED",
        "MANUALLY_COMPLETED",
    }:
        completed_at = latest_result["completed_at"]
        if completed_at:
            completed_time = datetime.strptime(
                completed_at,
                "%Y-%m-%d %H:%M:%S",
            )
            if 0 <= (now - completed_time).total_seconds() < 5:
                return latest_result["status"], latest_result

    if not is_system_time_configured():
        return "TIME_REQUIRED", None

    if state["blister_exhausted"]:
        return "BLISTER_EMPTY", None

    return "HOME", None


def build_display_status():
    now = datetime.now()
    active = get_active_schedule()
    blocking_result = get_unacknowledged_result()
    latest_result = get_latest_result()
    next_schedule = get_next_schedule()
    device_state = get_device_state()
    screen, screen_schedule = display_screen_state(
        now,
        active,
        blocking_result,
        latest_result,
        device_state,
    )
    target = screen_schedule or active
    if target is None or target["x_coordinate"] is None:
        target_coordinate = {
            "x": device_state["current_x"],
            "y": device_state["current_y"],
        }
    else:
        target_coordinate = {
            "x": target["x_coordinate"],
            "y": target["y_coordinate"],
        }

    recent_records = get_recent_records(3)
    uart_status = uart_service.get_status()
    settings = get_device_settings()
    local_ip = get_local_ip_address()

    return {
        "app_version": APP_VERSION,
        "screen": screen,
        "now": now.strftime("%H:%M:%S"),
        "date": now.strftime("%Y-%m-%d"),
        "time_ready": is_system_time_configured(),
        "schedule": serialize_schedule(screen_schedule),
        "active_schedule": serialize_schedule(active),
        "next_schedule": serialize_schedule(next_schedule),
        "remaining_seconds": seconds_until_deadline(
            screen_schedule or active,
            now,
        ),
        "target_coordinate": target_coordinate,
        "used_coordinates": [
            {"x": x_coordinate, "y": y_coordinate}
            for x_coordinate, y_coordinate in get_used_coordinates()
        ],
        "blister_exhausted": bool(device_state["blister_exhausted"]),
        "recent_records": [serialize_schedule(row) for row in recent_records],
        "settings": settings,
        "device": {
            "app": "OK",
            "database": "OK",
            "uart": uart_status["uart"],
            "uart_error": uart_status["last_error"],
            "network": "CONNECTED" if local_ip != "연결 안 됨" else "OFFLINE",
            "audio": (
                "DISABLED"
                if settings["voice_repeat"] == 0
                else "MUTED"
                if settings["volume_step"] == 0
                else "OK"
            ),
        },
    }


@app.get("/display")
def display_page():
    return render_template("display.html", app_version=APP_VERSION)


@app.get("/api/display-status")
def api_display_status():
    try:
        return jsonify(build_display_status())
    except sqlite3.Error as error:
        return jsonify({
            "screen": "DEVICE_ERROR",
            "error_code": "DB-READ-ERROR",
            "message": str(error),
        }), 500


@app.post("/api/display/dispense")
@display_local_only
def api_display_dispense():
    data = request.get_json(silent=True) or {}
    try:
        schedule_id = int(data.get("schedule_id"))
        schedule = uart_service.request_dispense(schedule_id)
    except (TypeError, ValueError, ActiveDoseError) as error:
        return jsonify({
            "error": "DISPENSE_NOT_ALLOWED",
            "message": str(error),
        }), 409
    return jsonify({"ok": True, "schedule": serialize_schedule(schedule)})


@app.post("/api/display/manual-complete")
@display_local_only
def api_display_manual_complete():
    data = request.get_json(silent=True) or {}
    try:
        schedule_id = int(data.get("schedule_id"))
    except (TypeError, ValueError):
        return jsonify({
            "error": "INVALID_SCHEDULE",
            "message": "올바른 복약 기록이 아닙니다.",
        }), 400

    result = complete_manual(
        schedule_id,
        datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
    )
    if result is None:
        return jsonify({
            "error": "MANUAL_COMPLETE_NOT_ALLOWED",
            "message": "배출 실패 상태에서만 수동 완료할 수 있습니다.",
        }), 409
    if result["blister_exhausted"]:
        play_blister_empty_voice()
    voice_alert_manager.stop()
    return jsonify({"ok": True, "blister_exhausted": result["blister_exhausted"]})


@app.post("/api/display/empty-blister-choice")
@display_local_only
def api_display_empty_blister_choice():
    data = request.get_json(silent=True) or {}
    try:
        schedule_id = int(data.get("schedule_id"))
    except (TypeError, ValueError):
        return jsonify({
            "error": "INVALID_SCHEDULE",
            "message": "올바른 복약 기록이 아닙니다.",
        }), 400

    choice = data.get("choice")
    event_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    try:
        if choice == "manual_taken":
            result = complete_empty_blister_manual(schedule_id, event_time)
            if result is None:
                raise ActiveDoseError("수동 복약을 기록할 수 없습니다.")
            schedule = get_schedule(schedule_id)
        elif choice == "manual_not_taken":
            schedule = uart_service.resume_empty_blister_dose(schedule_id)
        else:
            return jsonify({
                "error": "INVALID_CHOICE",
                "message": (
                    "수동복약 또는 수동미복약을 선택해 주세요."
                ),
            }), 400
    except ActiveDoseError as error:
        return jsonify({
            "error": "EMPTY_BLISTER_CHOICE_NOT_ALLOWED",
            "message": str(error),
        }), 409

    voice_alert_manager.stop()
    return jsonify({"ok": True, "schedule": serialize_schedule(schedule)})


@app.post("/api/display/acknowledge")
@display_local_only
def api_display_acknowledge():
    data = request.get_json(silent=True) or {}
    try:
        schedule_id = int(data.get("schedule_id"))
    except (TypeError, ValueError):
        return jsonify({"error": "INVALID_SCHEDULE"}), 400

    if not acknowledge_result(
        schedule_id,
        datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
    ):
        return jsonify({"error": "ACKNOWLEDGE_NOT_ALLOWED"}), 409
    voice_alert_manager.stop()
    return jsonify({"ok": True})


@app.post("/api/display/settings")
@display_local_only
def api_display_settings():
    data = request.get_json(silent=True) or {}
    try:
        update_device_settings(
            voice_repeat=data.get("voice_repeat"),
            volume_step=data.get("volume_step"),
            event_time=datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        )
    except (TypeError, ValueError) as error:
        return jsonify({
            "error": "INVALID_SETTINGS",
            "message": str(error),
        }), 400
    return jsonify({"ok": True, "settings": get_device_settings()})


@app.post("/api/display/test-volume")
@display_local_only
def api_display_test_volume():
    settings = get_device_settings()
    if not voice_alert_manager.test_once(settings["volume_step"]):
        return jsonify({
            "error": "VOLUME_MUTED",
            "message": "현재 음소거 상태입니다. 볼륨을 올려 주세요.",
        }), 409
    return jsonify({
        "ok": True,
        "volume_step": settings["volume_step"],
    })


@app.post("/api/display/reset-blister")
@display_local_only
def api_display_reset_blister():
    try:
        reset_blister(datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
    except ActiveDoseError as error:
        return jsonify({
            "error": "BLISTER_RESET_NOT_ALLOWED",
            "message": str(error),
        }), 409
    return jsonify({"ok": True})


def run_power_action(action):
    if action not in {"poweroff", "reboot"}:
        raise ValueError("지원하지 않는 전원 동작입니다.")
    if not POWER_HELPER_PATH.is_file():
        raise RuntimeError("전원 관리 도우미가 설치되지 않았습니다.")
    command = [str(POWER_HELPER_PATH), action]
    if os.geteuid() != 0:
        command = ["/usr/bin/sudo", "-n", *command]
    result = subprocess.run(
        command,
        capture_output=True,
        text=True,
        timeout=5,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            (result.stderr or result.stdout).strip()
            or "전원 동작을 실행하지 못했습니다."
        )


@app.post("/api/display/power")
@display_local_only
def api_display_power():
    data = request.get_json(silent=True) or {}
    try:
        run_power_action(data.get("action"))
    except (ValueError, RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        return jsonify({
            "error": "POWER_ACTION_FAILED",
            "message": str(error),
        }), 503
    return jsonify({"ok": True}), 202


if __name__ == "__main__":
    uart_service.start()

    app.run(
        host="0.0.0.0",
        port=5000,
        debug=False,
    )
