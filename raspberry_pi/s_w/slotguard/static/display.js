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
    const screensaverLayer = document.getElementById("lcd-screensaver");
    const blankScreenLayer = document.getElementById("lcd-blank-screen");

    const IDLE_SCREENSAVER_DELAY_MS = 5 * 60 * 1000;
    const IDLE_BLANK_DELAY_MS = 10 * 60 * 1000;
    const buttonSound = new Audio(window.SLOTGUARD_BUTTON_SOUND_URL);
    buttonSound.preload = "auto";

    const workflowScreens = new Set([
        "MOVING",
        "RETURNING_HOME",
        "RETURN_HOME_ERROR",
        "READY_TO_DISPENSE",
        "DISPENSING",
        "DISPENSED",
        "FAILED",
        "MANUALLY_COMPLETED",
        "BLISTER_EMPTY",
        "EMPTY_SLOT_CONFIRM",
        "EMPTY_BLISTER_CONFIRM",
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
    let networkRevealTimer = null;
    let networkRevealActive = false;
    let screensaverTimer = null;
    let blankScreenTimer = null;
    let idleMode = "active";
    let idleBlocked = true;
    let suppressWakeClickUntil = 0;

    function clearIdleTimers() {
        window.clearTimeout(screensaverTimer);
        window.clearTimeout(blankScreenTimer);
        screensaverTimer = null;
        blankScreenTimer = null;
    }

    function setIdleMode(mode) {
        idleMode = mode;
        screensaverLayer.hidden = mode !== "screensaver";
        blankScreenLayer.hidden = mode !== "blank";
    }

    function resetIdleCountdown() {
        clearIdleTimers();
        if (idleBlocked) {
            return;
        }
        screensaverTimer = window.setTimeout(() => {
            if (!idleBlocked) {
                setIdleMode("screensaver");
            }
        }, IDLE_SCREENSAVER_DELAY_MS);
        blankScreenTimer = window.setTimeout(() => {
            if (!idleBlocked) {
                setIdleMode("blank");
            }
        }, IDLE_BLANK_DELAY_MS);
    }

    function idleShouldBeBlocked() {
        return latestStatus === null
            || workflowScreens.has(latestStatus.screen)
            || !confirmLayer.hidden;
    }

    function updateIdleAvailability() {
        const blocked = idleShouldBeBlocked();
        if (blocked === idleBlocked) {
            return;
        }
        idleBlocked = blocked;
        if (blocked) {
            clearIdleTimers();
            setIdleMode("active");
        } else {
            resetIdleCountdown();
        }
    }

    function handleUserActivity(event) {
        if (idleMode !== "active") {
            event.preventDefault();
            event.stopImmediatePropagation();
            suppressWakeClickUntil = Date.now() + 600;
            setIdleMode("active");
        }
        resetIdleCountdown();
    }

    function playButtonSound(event) {
        const target = event.target;
        const button = target && target.closest
            ? target.closest("button")
            : null;
        if (!button || button.disabled || idleMode !== "active") {
            return;
        }
        const volumeStep = latestStatus && latestStatus.settings
            ? Number(latestStatus.settings.volume_step)
            : 5;
        if (!Number.isFinite(volumeStep) || volumeStep <= 0) {
            return;
        }
        buttonSound.pause();
        buttonSound.currentTime = 0;
        buttonSound.volume = Math.min(0.6, volumeStep * 0.06);
        const playback = buttonSound.play();
        if (playback && playback.catch) {
            playback.catch(() => {});
        }
    }

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

        if (status.screen === "RETURNING_HOME") {
            html = `
                <section class="lcd-workflow">
                    <div class="lcd-spinner" aria-hidden="true"></div>
                    <h1 class="lcd-title">기구가 원점으로 복귀 중입니다</h1>
                    <p>기구부에 손을 넣지 마세요</p>
                </section>`;
        } else if (status.screen === "RETURN_HOME_ERROR") {
            html = `
                <section class="lcd-workflow">
                    <h1 class="lcd-title">원점 복귀를 완료하지 못했습니다</h1>
                    <p>장치 통신과 기구부를 확인해 주세요</p>
                    <p>오류 코드: ${escapeHtml(schedule.home_error_code || "HOME-RETURN-ERROR")}</p>
                </section>`;
        } else if (status.screen === "MOVING") {
            const emptySlotMove = schedule
                && schedule.error_code === "EMPTY_BLISTER_SLOT";
            html = `
                <section class="lcd-workflow">
                    <div class="lcd-spinner" aria-hidden="true"></div>
                    <h1 class="lcd-title">${emptySlotMove
                        ? "현재 칸이 비어 있습니다.<br>다음 칸의 약으로 이동 중입니다."
                        : "약 위치로 이동 중입니다"}</h1>
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
        } else if (status.screen === "EMPTY_SLOT_CONFIRM") {
            html = `
                <section class="lcd-empty-choice lcd-empty-slot-choice">
                    <div class="lcd-empty-choice-info">
                        <h1>약 배출이 확인되지 않았습니다</h1>
                        <p>배출구를 확인하고 실제로 약이 나왔는지 선택해 주세요</p>
                    </div>
                    <div class="lcd-empty-choice-actions">
                        <button id="empty-slot-dispensed" type="button"
                                class="lcd-primary lcd-empty-choice-button">
                            약이 나왔음
                        </button>
                        <button id="empty-slot-empty" type="button"
                                class="lcd-secondary lcd-empty-choice-button">
                            약이 나오지 않음
                        </button>
                    </div>
                </section>`;
        } else if (status.screen === "EMPTY_BLISTER_CONFIRM") {
            html = `
                <section class="lcd-empty-choice">
                    <div class="lcd-empty-choice-info">
                        <h1>새 블리스터가 초기화되었습니다</h1>
                        <p>약을 직접 복용했는지 선택해 주세요</p>
                    </div>
                    <div class="lcd-empty-choice-actions">
                        <button id="empty-manual-taken" type="button"
                                class="lcd-primary lcd-empty-choice-button">
                            수동복약
                        </button>
                        <button id="empty-manual-not-taken" type="button"
                                class="lcd-secondary lcd-empty-choice-button">
                            수동미복약
                        </button>
                    </div>
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
                    <p>5초 후 자동으로 닫히며 다음 예약은 계속됩니다</p>
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
        const row = (label, value, good, id = "") => `
            <div class="lcd-device-row"${id ? ` id="${id}"` : ""}>
                <span>${escapeHtml(label)}</span>
                <strong class="${good ? "is-ok" : "is-error"}">${escapeHtml(value)}</strong>
            </div>`;
        screen.innerHTML = `
            <section class="lcd-device-panel">
                ${row("SLOT-GUARD", `정상 v${status.app_version}`, device.app === "OK")}
                ${row("ATmega UART", device.uart === "CONNECTED" ? "연결됨" : "확인 필요", device.uart === "CONNECTED")}
                ${row("데이터베이스", "정상", device.database === "OK")}
                ${row(
                    "네트워크",
                    device.network === "CONNECTED" ? "연결됨" : "오프라인",
                    device.network === "CONNECTED",
                    "network-status-row"
                )}
                ${row("장치 시간", status.time_ready ? "설정됨" : "설정 필요", status.time_ready)}
                ${row("음성", audioText, !audioDisabled)}
            </section>`;
        bindNetworkModeReveal();
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
        const emptyManualTaken = document.getElementById(
            "empty-manual-taken"
        );
        const emptyManualNotTaken = document.getElementById(
            "empty-manual-not-taken"
        );
        if (emptyManualTaken) {
            emptyManualTaken.addEventListener("click", async () => {
                emptyManualTaken.disabled = true;
                emptyManualNotTaken.disabled = true;
                await postJson("/api/display/empty-blister-choice", {
                    schedule_id: schedule.id,
                    choice: "manual_taken"
                });
                await refreshStatus();
            });
        }
        if (emptyManualNotTaken) {
            emptyManualNotTaken.addEventListener("click", async () => {
                emptyManualTaken.disabled = true;
                emptyManualNotTaken.disabled = true;
                await postJson("/api/display/empty-blister-choice", {
                    schedule_id: schedule.id,
                    choice: "manual_not_taken"
                });
                await refreshStatus();
            });
        }

        const emptySlotDispensed = document.getElementById(
            "empty-slot-dispensed"
        );
        const emptySlotEmpty = document.getElementById("empty-slot-empty");
        if (emptySlotDispensed) {
            emptySlotDispensed.addEventListener("click", async () => {
                emptySlotDispensed.disabled = true;
                emptySlotEmpty.disabled = true;
                await postJson("/api/display/empty-slot-choice", {
                    schedule_id: schedule.id,
                    choice: "dispensed"
                });
                await refreshStatus();
            });
        }
        if (emptySlotEmpty) {
            emptySlotEmpty.addEventListener("click", async () => {
                emptySlotDispensed.disabled = true;
                emptySlotEmpty.disabled = true;
                await postJson("/api/display/empty-slot-choice", {
                    schedule_id: schedule.id,
                    choice: "empty"
                });
                await refreshStatus();
            });
        }
    }

    function render(status) {
        latestStatus = status;
        updateIdleAvailability();
        headerTime.textContent = status.now ? status.now.slice(0, 5) : "--:--";
        const deviceOk = status.device
            && status.device.uart === "CONNECTED"
            && status.time_ready;
        headerDevice.textContent = deviceOk ? "장치 정상" : "상태 확인";
        headerDevice.className = `lcd-device-state ${deviceOk ? "is-ok" : "is-error"}`;

        const locked = workflowScreens.has(status.screen);
        if (locked && networkRevealActive) {
            clearNetworkModeReveal();
        }
        nav.classList.toggle("is-locked", locked);
        nav.querySelectorAll("button").forEach((button) => {
            button.classList.toggle("is-active", !locked && button.dataset.view === localView);
        });

        if (status.screen === "BLISTER_EMPTY") {
            renderBlisterEmpty(status);
        } else if (locked) {
            renderWorkflow(status);
        } else if (localView === "device") {
            if (!networkRevealActive) {
                renderDevice(status);
            }
        } else if (localView === "settings") {
            renderSettings(status);
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

    async function getJson(url) {
        const response = await fetch(url, {cache: "no-store"});
        const result = await response.json().catch(() => ({}));
        if (!response.ok) {
            const error = new Error(result.message || "요청을 처리하지 못했습니다.");
            showToast(error.message);
            throw error;
        }
        return result;
    }

    function networkModeLabel(mode) {
        if (mode === "operation") {
            return "운영 모드";
        }
        if (mode === "development") {
            return "개발 모드";
        }
        return "확인 불가";
    }

    async function switchNetworkMode(mode) {
        showToast(`${networkModeLabel(mode)}로 전환 중입니다.`);
        const result = await postJson("/api/display/network-mode", {mode});
        showToast(`${networkModeLabel(result.mode)}로 전환했습니다.`);
        await refreshStatus();
    }

    async function openNetworkModeConfirmation() {
        try {
            const result = await getJson("/api/display/network-mode");
            const targetMode = result.mode === "operation"
                ? "development"
                : "operation";
            const targetLabel = networkModeLabel(targetMode);
            const message = result.mode === "operation"
                ? `현재 운영 모드입니다. PC 핫스팟을 켠 뒤 ${targetLabel}로 전환하세요.`
                : `현재 ${networkModeLabel(result.mode)}입니다. SLOT-GUARD AP를 켜는 ${targetLabel}로 전환합니다.`;
            openHoldConfirmation(
                `${targetLabel} 전환`,
                message,
                () => switchNetworkMode(targetMode)
            );
        } catch (error) {
            // getJson already shows the user-facing error.
        }
    }

    function clearNetworkModeReveal() {
        window.clearTimeout(networkRevealTimer);
        networkRevealTimer = null;
        networkRevealActive = false;
    }

    function bindNetworkModeReveal() {
        const networkRow = document.getElementById("network-status-row");
        if (!networkRow) {
            return;
        }

        networkRow.addEventListener("contextmenu", (event) => {
            event.preventDefault();
        });
        networkRow.addEventListener("pointerdown", (event) => {
            if (networkRevealTimer !== null) {
                return;
            }
            event.preventDefault();
            networkRevealActive = true;
            if (networkRow.setPointerCapture) {
                networkRow.setPointerCapture(event.pointerId);
            }
            networkRevealTimer = window.setTimeout(() => {
                networkRevealTimer = null;
                networkRevealActive = false;
                openNetworkModeConfirmation();
            }, 5000);
        });
        ["pointerup", "pointercancel", "pointerleave"].forEach((eventName) => {
            networkRow.addEventListener(eventName, clearNetworkModeReveal);
        });
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
        updateIdleAvailability();
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
        updateIdleAvailability();
    }

    document.addEventListener("pointerdown", handleUserActivity, true);
    document.addEventListener("keydown", handleUserActivity, true);
    document.addEventListener("pointerdown", playButtonSound, true);
    document.addEventListener("click", (event) => {
        if (Date.now() < suppressWakeClickUntil) {
            event.preventDefault();
            event.stopImmediatePropagation();
        }
    }, true);

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
            clearNetworkModeReveal();
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
