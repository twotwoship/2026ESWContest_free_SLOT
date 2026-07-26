import json
import secrets
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
    MAX_ALLOWED_SECONDS,
    create_schedule,
    delete_schedule,
    get_current_coordinate,
    get_schedules,
    init_db,
    reset_schedules_and_position,
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
SESSION_IDLE_TIMEOUT_SECONDS = 30 * 60


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


def play_voice(voice_file, description):
    if not voice_file.is_file():
        print(f"[VOICE] 음성 파일을 찾을 수 없습니다: {voice_file}")
        return

    result = subprocess.run(
        [
            "/usr/bin/cvlc",
            "--intf", "dummy",
            "--play-and-exit",
            "--no-video",
            "-A", "alsa",
            "--alsa-audio-device", "sysdefault:CARD=Headphones",
            str(voice_file),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )

    if result.returncode != 0:
        print(
            f"[VOICE] {description} 재생 실패: "
            f"returncode={result.returncode}"
        )


def play_voice_async(voice_file, description):
    threading.Thread(
        target=play_voice,
        args=(voice_file, description),
        daemon=True,
    ).start()


def play_medicine_voice():
    play_voice_async(MEDICINE_VOICE_FILE, "복약시간 안내")


def play_blister_empty_voice():
    play_voice_async(BLISTER_EMPTY_VOICE_FILE, "블리스터 소진 안내")


uart_service = UartService(
    on_schedule_started=play_medicine_voice,
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

        if status == "WAITING":
            counts["waiting"] += 1
        elif status == "ALLOWED":
            counts["allowed"] += 1
        elif status == "DISPENSED":
            counts["dispensed"] += 1
        elif status == "MISSED":
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
            allowed_seconds = parse_allowed_seconds(request.form)

            scheduled_at = datetime.strptime(
                f"{scheduled_date} {scheduled_time}",
                "%Y-%m-%d %H:%M",
            ).strftime("%Y-%m-%d %H:%M")

            create_schedule(
                scheduled_at=scheduled_at,
                allowed_seconds=allowed_seconds,
            )

            return redirect(url_for("schedule_page"))

        except (KeyError, ValueError):
            error = (
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


if __name__ == "__main__":
    uart_service.start()

    app.run(
        host="0.0.0.0",
        port=5000,
        debug=False,
    )
