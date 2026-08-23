(function () {
    "use strict";

    const screen = document.getElementById("lcd-screen");
    const nav = document.getElementById("lcd-nav");
    const headerTime = document.getElementById("header-time");
    const headerDevice = document.getElementById("header-device");
    const headerVolumeTest = document.getElementById("header-volume-test");
    const toast = document.getElementById("lcd-toast");
    const confirmLayer = document.getElementById("lcd-confirm");
    const confirmTitle = document.getElementById("confirm-title");
    const confirmMessage = document.getElementById("confirm-message");
    const confirmCancel = document.getElementById("confirm-cancel");
    const confirmHold = document.getElementById("confirm-hold");

    const workflowScreens = new Set([
        "MOVING",
        "READY_TO_DISPENSE",
        "DISPENSING",
        "DISPENSED",
        "FAILED",
        "MANUALLY_COMPLETED",
        "MISSED",
        "COMM_ERROR",
        "DEVICE_ERROR"
    ]);
    const statusLabels = {
        DISPENSED: "배출 확인",
        MANUALLY_COMPLETED: "수동 완료",
        FAILED: "배출 실패",
        MISSED: "미복약",
        COMM_ERROR: "통신 오류"
    };
    let latestStatus = null;
    let localView = "home";
    let toastTimer = null;
    let holdTimer = null;
    let holdAction = null;

    function escapeHtml(value) {
        return String(value ?? "").replace(/[&<>'"]/g, (character) => ({
            "&": "&amp;",
            "<": "&lt;",
            ">": "&gt;",
            "'": "&#39;",
            '"': "&quot;"
        })[character]);
    }

    function slotNumber(coordinate) {
        if (!coordinate) {
            return "-";
        }
        return coordinate.x * 5 + coordinate.y + 1;
    }

    function formatRemaining(seconds) {
        if (seconds === null || seconds === undefined) {
            return "";
        }
        if (seconds <= 0) {
            return "허용시간 종료";
        }
        const hours = Math.floor(seconds / 3600);
        const minutes = Math.floor((seconds % 3600) / 60);
        if (hours > 0) {
            return `${hours}시간 ${minutes}분 남음`;
        }
        return `${Math.max(1, minutes)}분 남음`;
    }

    function nextTimeText(schedule) {
        if (!schedule) {
            return "--:--";
        }
        return schedule.scheduled_at.slice(11, 16);
    }

    function recordDateTime(record) {
        const value = record.completed_at || record.scheduled_at || "";
        const matched = value.match(/^\d{4}-(\d{2})-(\d{2})[ T](\d{2}):(\d{2})/);
        if (!matched) {
            return "--일 --:--";
        }
        return `${Number(matched[2])}일 ${matched[3]}:${matched[4]}`;
    }

    function recordRows(records) {
        if (!records.length) {
            return '<div class="lcd-record-empty">최근 기록 없음</div>';
        }
        return records.map((record) => {
            const statusClass = record.status === "DISPENSED"
                ? "is-good"
                : record.status === "MANUALLY_COMPLETED"
                    ? "is-manual"
                    : "is-bad";
            return `
                <div class="lcd-record-row">
                    <span>${escapeHtml(recordDateTime(record))}</span>
                    <span>${escapeHtml(record.medicine_name)}</span>
                    <span class="lcd-record-status ${statusClass}">
                        ${escapeHtml(statusLabels[record.status] || record.status)}
                    </span>
                </div>`;
        }).join("");
    }

    function slotGrid(status, emphasizeTarget = true) {
        const used = new Set(
            status.used_coordinates.map((coordinate) => `${coordinate.x}${coordinate.y}`)
        );
        const targetKey = `${status.target_coordinate.x}${status.target_coordinate.y}`;
        let cells = "";
        for (let x = 0; x < 2; x += 1) {
            for (let y = 0; y < 5; y += 1) {
                const key = `${x}${y}`;
                const classes = ["lcd-slot"];
                if (used.has(key)) {
                    classes.push("is-used");
                } else if (emphasizeTarget && key === targetKey) {
                    classes.push("is-target");
                }
                cells += `<div class="${classes.join(" ")}"><span>${x * 5 + y + 1}</span></div>`;
            }
        }
        return `<div class="lcd-slot-grid">${cells}</div>`;
    }

    function renderHome(status) {
        const next = status.next_schedule;
        const medicine = next ? next.medicine_name : "등록된 일정 없음";
        const now = new Date(`${status.date}T${status.now}`);
        let remaining = "관리자 웹에서 예약해 주세요";
        if (next && !Number.isNaN(now.getTime())) {
            const due = new Date(next.scheduled_at.replace(" ", "T"));
            const seconds = Math.max(0, Math.floor((due - now) / 1000));
            remaining = seconds > 86400
                ? `${Math.ceil(seconds / 86400)}일 후`
                : formatRemaining(seconds);
        }
        const remainingHtml = next
            ? escapeHtml(remaining)
            : "관리자 웹에서<br>예약해 주세요";
        screen.innerHTML = `
            <section class="lcd-home">
                <div class="lcd-next">
                    <div class="lcd-next-header">
                        <div class="lcd-kicker">다음 등록 일정</div>
                        <div class="lcd-next-time">${escapeHtml(nextTimeText(next))}</div>
                    </div>
                    <div class="lcd-next-details">
                        <div class="lcd-medicine">${escapeHtml(medicine)}</div>
                        <div class="lcd-remaining">${remainingHtml}</div>
                    </div>
                </div>
                <div class="lcd-records">
                    <h2 class="lcd-section-title">최근 기록</h2>
                    <div class="lcd-record-list">
                        ${recordRows(status.recent_records)}
                    </div>
                </div>
            </section>`;
    }

    function workflowHeader(schedule, targetCoordinate) {
        if (!schedule || !targetCoordinate) {
            return "";
        }
        const medicine = schedule ? escapeHtml(schedule.medicine_name) : "약";
        return `<div class="lcd-highlight">${medicine} · ${slotNumber(targetCoordinate)}번 슬롯</div>`;
    }

    function renderWorkflow(status) {
        const schedule = status.schedule || status.active_schedule;
        const common = workflowHeader(schedule, status.target_coordinate);
        const remaining = escapeHtml(formatRemaining(status.remaining_seconds));
        let html = "";

        if (status.screen === "MOVING") {
            html = `
                <section class="lcd-workflow">
                    <div class="lcd-spinner" aria-hidden="true"></div>
                    <h1 class="lcd-title">약 위치로 이동 중입니다</h1>
                    ${common}
                    <p>기구부에 손을 넣지 마세요 · ${remaining}</p>
                </section>`;
        } else if (status.screen === "READY_TO_DISPENSE") {
            const medicine = schedule ? escapeHtml(schedule.medicine_name) : "약";
            const targetSlot = slotNumber(status.target_coordinate);
            html = `
                <section class="lcd-workflow lcd-ready">
                    <div class="lcd-ready-info">
                        <h1 class="lcd-ready-title">복약 시간입니다</h1>
                        <span class="lcd-ready-medicine">
                            ${medicine} · ${targetSlot}번 슬롯
                        </span>
                        <span class="lcd-ready-remaining">${remaining}</span>
                    </div>
                    <button id="dispense-button" type="button"
                            class="lcd-primary lcd-dispense-button">
                        약 배출
                    </button>
                </section>`;
        } else if (status.screen === "DISPENSING") {
            html = `
                <section class="lcd-workflow">
                    <div class="lcd-spinner" aria-hidden="true"></div>
                    <h1 class="lcd-title">약을 배출하고 있습니다</h1>
                    ${common}
                    <p>서보모터 동작 중에는 블리스터를 빼지 마세요</p>
                </section>`;
        } else if (status.screen === "DISPENSED") {
            html = `
                <section class="lcd-workflow">
                    <div class="lcd-icon" aria-hidden="true">✓</div>
                    <h1 class="lcd-title">정제 배출이 확인되었습니다</h1>
                    ${common}
                    <p>잠시 후 홈 화면으로 돌아갑니다</p>
                </section>`;
        } else if (status.screen === "MANUALLY_COMPLETED") {
            html = `
                <section class="lcd-workflow">
                    <div class="lcd-icon" aria-hidden="true">✓</div>
                    <h1 class="lcd-title">수동 복약 완료를 기록했습니다</h1>
                    ${common}
                    <p>잠시 후 홈 화면으로 돌아갑니다</p>
                </section>`;
        } else if (status.screen === "FAILED") {
            html = `
                <section class="lcd-workflow">
                    <h1 class="lcd-title">약 배출을 확인하지 못했습니다</h1>
                    ${common}
                    <p>직접 약을 복용한 경우에만 완료를 눌러 주세요</p>
                    <div class="lcd-action-row">
                        <button id="manual-complete" type="button" class="lcd-primary">
                            수동 복약 완료
                        </button>
                        <button id="result-ack" type="button" class="lcd-secondary">
                            복용하지 못함
                        </button>
                    </div>
                </section>`;
        } else if (status.screen === "MISSED") {
            html = `
                <section class="lcd-workflow">
                    <div class="lcd-icon" aria-hidden="true">!</div>
                    <h1 class="lcd-title">복약 허용시간이 지났습니다</h1>
                    ${common}
                    <button id="result-ack" type="button" class="lcd-secondary">화면 확인</button>
                </section>`;
        } else if (status.screen === "COMM_ERROR") {
            html = `
                <section class="lcd-workflow">
                    <div class="lcd-icon" aria-hidden="true">!</div>
                    <h1 class="lcd-title">장치 통신을 확인해 주세요</h1>
                    ${common}
                    <p>오류 코드: ${escapeHtml(schedule.error_code || "UART-ERROR")}</p>
                    <button id="result-ack" type="button" class="lcd-secondary">화면 확인</button>
                </section>`;
        } else if (status.screen === "TIME_REQUIRED") {
            html = `
                <section class="lcd-workflow">
                    <div class="lcd-icon" aria-hidden="true">!</div>
                    <h1 class="lcd-title">장치 시간을 설정해 주세요</h1>
                    <p>관리자 웹에서 스마트폰 시간으로 동기화해야<br>예약과 배출 기능이 시작됩니다</p>
                </section>`;
        } else if (status.screen === "DEVICE_ERROR") {
            html = `
                <section class="lcd-workflow">
                    <div class="lcd-icon" aria-hidden="true">!</div>
                    <h1 class="lcd-title">장치 상태를 확인해 주세요</h1>
                    <p>오류 코드: ${escapeHtml(status.error_code || "DEVICE-ERROR")}</p>
                </section>`;
        }

        screen.innerHTML = html;
        bindWorkflowActions(status, schedule);
    }

    function renderBlisterEmpty(status) {
        screen.innerHTML = `
            <section class="lcd-workflow">
                <div class="lcd-icon" aria-hidden="true">▦</div>
                <h1 class="lcd-title">새 블리스터가 필요합니다</h1>
                <p>새 블리스터를 넣은 뒤 초기화해 주세요</p>
                <button id="empty-reset" type="button" class="lcd-primary">새 블리스터 초기화</button>
            </section>`;
        document.getElementById("empty-reset").addEventListener("click", () => {
            openHoldConfirmation(
                "새 블리스터 초기화",
                "새 블리스터가 장착되었는지 확인해 주세요. 미래 예약은 유지됩니다.",
                resetBlister
            );
        });
    }

    function renderDevice(status) {
        const device = status.device;
        const audioDisabled = device.audio === "DISABLED";
        const audioText = audioDisabled
            ? "미사용"
            : device.audio === "MUTED"
                ? "음소거"
                : "사용";
        const row = (label, value, good) => `
            <div class="lcd-device-row">
                <span>${escapeHtml(label)}</span>
                <strong class="${good ? "is-ok" : "is-error"}">${escapeHtml(value)}</strong>
            </div>`;
        screen.innerHTML = `
            <section class="lcd-device-panel">
                ${row("SLOT-GUARD", `정상 v${status.app_version}`, device.app === "OK")}
                ${row("ATmega UART", device.uart === "CONNECTED" ? "연결됨" : "확인 필요", device.uart === "CONNECTED")}
                ${row("데이터베이스", "정상", device.database === "OK")}
                ${row("네트워크", device.network === "CONNECTED" ? "연결됨" : "오프라인", true)}
                ${row("장치 시간", status.time_ready ? "설정됨" : "설정 필요", status.time_ready)}
                ${row("음성", audioText, !audioDisabled)}
            </section>`;
    }

    function renderSettings(status) {
        const settings = status.settings;
        const volumeText = settings.volume_step === 0
            ? "음소거"
            : settings.volume_step === 10
                ? "최대"
                : String(settings.volume_step);
        screen.innerHTML = `
            <section class="lcd-settings-panel">
                <div class="lcd-setting-line">
                    <strong>음성 반복</strong>
                    <div class="lcd-stepper">
                        <button id="repeat-minus" type="button" aria-label="반복 횟수 줄이기">−</button>
                        <output id="repeat-value">${settings.voice_repeat}회</output>
                        <button id="repeat-plus" type="button" aria-label="반복 횟수 늘리기">＋</button>
                    </div>
                </div>
                <div class="lcd-setting-line lcd-volume-line">
                    <strong>볼륨 크기</strong>
                    <div class="lcd-stepper">
                        <button id="volume-minus" type="button" aria-label="볼륨 줄이기">−</button>
                        <output id="volume-value">${volumeText}</output>
                        <button id="volume-plus" type="button" aria-label="볼륨 늘리기">＋</button>
                    </div>
                </div>
                <div class="lcd-system-actions">
                    <button id="settings-reset" type="button" class="lcd-secondary">블리스터 초기화</button>
                    <button id="settings-reboot" type="button" class="lcd-secondary">재시작</button>
                    <button id="settings-poweroff" type="button" class="lcd-danger">시스템 종료</button>
                </div>
            </section>`;

        document.getElementById("repeat-minus").addEventListener("click", () => {
            saveSettings(Math.max(0, latestStatus.settings.voice_repeat - 1), latestStatus.settings.volume_step);
        });
        document.getElementById("repeat-plus").addEventListener("click", () => {
            saveSettings(Math.min(10, latestStatus.settings.voice_repeat + 1), latestStatus.settings.volume_step);
        });
        document.getElementById("volume-minus").addEventListener("click", () => {
            saveSettings(latestStatus.settings.voice_repeat, Math.max(0, latestStatus.settings.volume_step - 1));
        });
        document.getElementById("volume-plus").addEventListener("click", () => {
            saveSettings(latestStatus.settings.voice_repeat, Math.min(10, latestStatus.settings.volume_step + 1));
        });
        document.getElementById("settings-reset").addEventListener("click", () => {
            openHoldConfirmation(
                "새 블리스터 초기화",
                "새 블리스터가 장착되었는지 확인해 주세요. 미래 예약은 유지됩니다.",
                resetBlister
            );
        });
        document.getElementById("settings-reboot").addEventListener("click", () => {
            openHoldConfirmation(
                "장치 재시작",
                "진행 중인 복약이 없는지 확인해 주세요.",
                () => powerAction("reboot")
            );
        });
        document.getElementById("settings-poweroff").addEventListener("click", () => {
            openHoldConfirmation(
                "시스템 종료",
                "종료 후 다시 켜려면 전원을 다시 연결해야 합니다.",
                () => powerAction("poweroff")
            );
        });
    }

    function bindWorkflowActions(status, schedule) {
        const dispenseButton = document.getElementById("dispense-button");
        if (dispenseButton) {
            dispenseButton.addEventListener("click", async () => {
                dispenseButton.disabled = true;
                await postJson("/api/display/dispense", {schedule_id: schedule.id});
                await refreshStatus();
            });
        }
        const manualButton = document.getElementById("manual-complete");
        if (manualButton) {
            manualButton.addEventListener("click", () => {
                openHoldConfirmation(
                    "수동 복약 완료",
                    "블리스터를 직접 꺼내 약을 복용하고 다시 장착한 경우에만 실행하세요.",
                    async () => {
                        await postJson("/api/display/manual-complete", {schedule_id: schedule.id});
                        await refreshStatus();
                    }
                );
            });
        }
        const resultAck = document.getElementById("result-ack");
        if (resultAck) {
            resultAck.addEventListener("click", async () => {
                await postJson("/api/display/acknowledge", {schedule_id: schedule.id});
                localView = "home";
                await refreshStatus();
            });
        }
    }

    function render(status) {
        latestStatus = status;
        headerTime.textContent = status.now ? status.now.slice(0, 5) : "--:--";
        const deviceOk = status.device
            && status.device.uart === "CONNECTED"
            && status.time_ready;
        headerDevice.textContent = deviceOk ? "장치 정상" : "상태 확인";
        headerDevice.className = `lcd-device-state ${deviceOk ? "is-ok" : "is-error"}`;

        const locked = workflowScreens.has(status.screen);
        nav.classList.toggle("is-locked", locked);
        nav.querySelectorAll("button").forEach((button) => {
            button.classList.toggle("is-active", !locked && button.dataset.view === localView);
        });

        if (locked) {
            renderWorkflow(status);
        } else if (localView === "device") {
            renderDevice(status);
        } else if (localView === "settings") {
            renderSettings(status);
        } else if (status.screen === "BLISTER_EMPTY") {
            renderBlisterEmpty(status);
        } else if (status.screen === "TIME_REQUIRED") {
            renderWorkflow(status);
        } else {
            renderHome(status);
        }
    }

    async function postJson(url, payload) {
        const response = await fetch(url, {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify(payload)
        });
        const result = await response.json().catch(() => ({}));
        if (!response.ok) {
            const error = new Error(result.message || "요청을 처리하지 못했습니다.");
            showToast(error.message);
            throw error;
        }
        return result;
    }

    async function saveSettings(voiceRepeat, volumeStep) {
        try {
            const result = await postJson("/api/display/settings", {
                voice_repeat: voiceRepeat,
                volume_step: volumeStep
            });
            latestStatus.settings = result.settings;
            renderSettings(latestStatus);
        } catch (error) {
            // postJson already shows the user-facing error.
        }
    }

    async function resetBlister() {
        await postJson("/api/display/reset-blister", {});
        localView = "home";
        await refreshStatus();
    }

    async function powerAction(action) {
        await postJson("/api/display/power", {action});
        screen.innerHTML = `
            <section class="lcd-workflow">
                <div class="lcd-spinner" aria-hidden="true"></div>
                <h1 class="lcd-title">${action === "reboot" ? "장치를 재시작합니다" : "안전하게 종료합니다"}</h1>
                <p>잠시 기다려 주세요</p>
            </section>`;
        nav.classList.add("is-locked");
    }

    function showToast(message) {
        window.clearTimeout(toastTimer);
        toast.textContent = message;
        toast.hidden = false;
        toastTimer = window.setTimeout(() => {
            toast.hidden = true;
        }, 3500);
    }

    function openHoldConfirmation(title, message, action) {
        confirmTitle.textContent = title;
        confirmMessage.textContent = message;
        confirmHold.textContent = "2초간 눌러 실행";
        holdAction = action;
        confirmLayer.hidden = false;
    }

    function clearHold() {
        window.clearTimeout(holdTimer);
        holdTimer = null;
        confirmHold.textContent = "2초간 눌러 실행";
    }

    function closeConfirmation() {
        clearHold();
        holdAction = null;
        confirmLayer.hidden = true;
    }

    confirmCancel.addEventListener("click", closeConfirmation);
    ["pointerup", "pointercancel", "pointerleave"].forEach((eventName) => {
        confirmHold.addEventListener(eventName, clearHold);
    });
    confirmHold.addEventListener("pointerdown", () => {
        if (holdTimer !== null) {
            return;
        }
        confirmHold.textContent = "계속 누르세요…";
        holdTimer = window.setTimeout(async () => {
            const action = holdAction;
            closeConfirmation();
            if (action) {
                try {
                    await action();
                } catch (error) {
                    // postJson already shows the user-facing error.
                }
            }
        }, 2000);
    });

    nav.querySelectorAll("button").forEach((button) => {
        button.addEventListener("click", () => {
            localView = button.dataset.view;
            if (latestStatus) {
                render(latestStatus);
            }
        });
    });

    headerVolumeTest.addEventListener("click", async () => {
        headerVolumeTest.disabled = true;
        headerVolumeTest.textContent = "재생 중";
        try {
            await postJson("/api/display/test-volume", {});
        } catch (error) {
            // postJson already shows the user-facing error.
        } finally {
            window.setTimeout(() => {
                headerVolumeTest.disabled = false;
                headerVolumeTest.textContent = "볼륨 테스트";
            }, 2000);
        }
    });

    async function refreshStatus() {
        try {
            const response = await fetch("/api/display-status", {cache: "no-store"});
            const status = await response.json();
            if (!response.ok && !status.screen) {
                throw new Error("상태 정보를 읽지 못했습니다.");
            }
            render(status);
        } catch (error) {
            headerDevice.textContent = "연결 오류";
            headerDevice.className = "lcd-device-state is-error";
            showToast(error.message || "SLOT-GUARD 앱 연결을 확인해 주세요.");
        }
    }

    refreshStatus();
    window.setInterval(refreshStatus, 1000);
}());
