#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "root 권한으로 실행하십시오." >&2
    exit 1
fi
if [ "$#" -gt 1 ]; then
    echo "사용법: sudo ./system/test_slotguard_ap.sh [자동복귀초]" >&2
    exit 2
fi

rollback_seconds=${1:-300}
case "$rollback_seconds" in
    *[!0-9]*|'')
        echo "자동복귀초는 숫자여야 합니다." >&2
        exit 2
        ;;
esac
if [ "$rollback_seconds" -lt 60 ] || [ "$rollback_seconds" -gt 600 ]; then
    echo "자동복귀 시간은 60~600초만 허용합니다." >&2
    exit 2
fi

config_path=/etc/slotguard/network.conf
if [ ! -r "$config_path" ]; then
    echo "네트워크 helper 설정을 찾을 수 없습니다." >&2
    exit 3
fi

wifi_interface=$(
    sed -n 's/^[[:space:]]*interface[[:space:]]*=[[:space:]]*//p' \
        "$config_path"
)
development_connection=$(
    sed -n 's/^[[:space:]]*development_connection[[:space:]]*=[[:space:]]*//p' \
        "$config_path"
)
if [ -z "$wifi_interface" ] || [ -z "$development_connection" ]; then
    echo "네트워크 helper 설정이 올바르지 않습니다." >&2
    exit 3
fi
if ! /usr/bin/nmcli -g connection.id connection show slotguard-ap \
        >/dev/null 2>&1; then
    echo "slotguard-ap 프로필이 설치되지 않았습니다." >&2
    exit 3
fi
if [ "$(/usr/local/sbin/slotguard-network status)" != development ]; then
    echo "시험은 개발 모드에서만 시작할 수 있습니다." >&2
    exit 4
fi

activation_unit=slotguard-network-test-activate
rollback_unit=slotguard-network-test-rollback

/usr/bin/systemd-run \
    --quiet \
    --collect \
    --unit "$rollback_unit" \
    --on-active="${rollback_seconds}s" \
    --timer-property=AccuracySec=1s \
    /usr/bin/nmcli --wait 20 connection up "$development_connection" \
    ifname "$wifi_interface"

/usr/bin/systemd-run \
    --quiet \
    --collect \
    --unit "$activation_unit" \
    --on-active=10s \
    --timer-property=AccuracySec=1s \
    /usr/local/sbin/slotguard-network operation

echo "10초 뒤 SLOT-GUARD AP 시험을 시작합니다."
echo "${rollback_seconds}초 뒤 개발 모드로 자동 복귀합니다."
