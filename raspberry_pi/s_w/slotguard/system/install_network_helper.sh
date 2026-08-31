#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "root 권한으로 실행하십시오." >&2
    exit 1
fi

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "사용법: sudo ./system/install_network_helper.sh <Flask실행사용자> <개발Wi-Fi프로필> [인터페이스]" >&2
    exit 2
fi

app_user=$1
development_connection=$2
wifi_interface=${3:-wlan0}

case "$app_user" in
    *[!A-Za-z0-9_-]*|'')
        echo "올바르지 않은 사용자 이름입니다." >&2
        exit 2
        ;;
esac
case "$wifi_interface" in
    *[!A-Za-z0-9_.:-]*|'')
        echo "올바르지 않은 Wi-Fi 인터페이스 이름입니다." >&2
        exit 2
        ;;
esac

case "$development_connection" in
    *[!A-Za-z0-9_.:+@\ -]*|'')
        echo "올바르지 않은 개발 Wi-Fi 프로필 이름입니다." >&2
        exit 2
        ;;
esac
if ! getent passwd "$app_user" >/dev/null; then
    echo "존재하지 않는 사용자입니다: $app_user" >&2
    exit 2
fi
if ! /usr/bin/nmcli -g connection.id connection show "$development_connection" \
        >/dev/null 2>&1; then
    echo "개발 Wi-Fi 프로필을 찾을 수 없습니다: $development_connection" >&2
    exit 3
fi
if ! /usr/bin/nmcli -g GENERAL.TYPE device show "$wifi_interface" \
        >/dev/null 2>&1; then
    echo "Wi-Fi 인터페이스를 찾을 수 없습니다: $wifi_interface" >&2
    exit 3
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
helper_source="$script_dir/slotguard_network_helper.py"
helper_target=/usr/local/sbin/slotguard-network
config_dir=/etc/slotguard
config_target="$config_dir/network.conf"
sudoers_target=/etc/sudoers.d/slotguard-network
config_temp=$(mktemp)
sudoers_temp=$(mktemp)

trap 'rm -f "$config_temp" "$sudoers_temp"' EXIT

install -o root -g root -m 0755 "$helper_source" "$helper_target"
install -d -o root -g root -m 0755 "$config_dir"

printf '%s\n' \
    '[network]' \
    "interface = $wifi_interface" \
    'operation_connection = slotguard-ap' \
    "development_connection = $development_connection" \
    > "$config_temp"
install -o root -g root -m 0600 "$config_temp" "$config_target"

printf '%s ALL=(root) NOPASSWD: %s status, %s operation, %s development\n' \
    "$app_user" \
    "$helper_target" \
    "$helper_target" \
    "$helper_target" \
    > "$sudoers_temp"
chmod 0440 "$sudoers_temp"
visudo -cf "$sudoers_temp"
install -o root -g root -m 0440 "$sudoers_temp" "$sudoers_target"

echo "SLOT-GUARD 네트워크 전환 도우미 설치 완료"
echo "현재 연결은 변경하지 않았습니다: $development_connection"
