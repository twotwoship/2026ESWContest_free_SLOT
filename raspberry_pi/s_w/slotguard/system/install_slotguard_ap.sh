#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "root 권한으로 실행하십시오." >&2
    exit 1
fi
if [ "$#" -gt 1 ]; then
    echo "사용법: sudo ./system/install_slotguard_ap.sh [인터페이스]" >&2
    exit 2
fi

wifi_interface=${1:-wlan0}
connection_name=slotguard-ap
ssid=SLOT-GUARD
config_path=/etc/slotguard/network.conf

case "$wifi_interface" in
    *[!A-Za-z0-9_.:-]*|'')
        echo "올바르지 않은 Wi-Fi 인터페이스 이름입니다." >&2
        exit 2
        ;;
esac
if /usr/bin/nmcli -g connection.id connection show "$connection_name" \
        >/dev/null 2>&1; then
    echo "이미 AP 프로필이 존재합니다: $connection_name" >&2
    exit 3
fi
if [ ! -r "$config_path" ]; then
    echo "네트워크 helper 설정을 찾을 수 없습니다." >&2
    exit 3
fi
development_connection=$(
    sed -n 's/^[[:space:]]*development_connection[[:space:]]*=[[:space:]]*//p' \
        "$config_path"
)
if [ -z "$development_connection" ]; then
    echo "개발 Wi-Fi 프로필 설정이 올바르지 않습니다." >&2
    exit 3
fi
if ! /usr/bin/nmcli -g connection.id connection show "$development_connection" \
        >/dev/null 2>&1; then
    echo "개발 Wi-Fi 프로필을 찾을 수 없습니다: $development_connection" >&2
    exit 3
fi

printf 'SLOT-GUARD Wi-Fi 비밀번호(8~63자): ' >&2
if [ -t 0 ]; then
    stty -echo
    trap 'stty echo 2>/dev/null || :' EXIT HUP INT TERM
fi
IFS= read -r ap_password
if [ -t 0 ]; then
    stty echo
    trap - EXIT HUP INT TERM
    printf '\n' >&2
fi

password_length=${#ap_password}
if [ "$password_length" -lt 8 ] || [ "$password_length" -gt 63 ]; then
    echo "Wi-Fi 비밀번호는 8~63자여야 합니다." >&2
    exit 2
fi
case "$ap_password" in
    *[!A-Za-z0-9_.:!+@-]*)
        echo "비밀번호에는 영문, 숫자, _ . : ! + @ - 만 사용할 수 있습니다." >&2
        exit 2
        ;;
esac

cleanup_profile() {
    /usr/bin/nmcli connection delete "$connection_name" >/dev/null 2>&1 || :
}
trap cleanup_profile EXIT HUP INT TERM

/usr/bin/nmcli connection add \
    type wifi \
    ifname "$wifi_interface" \
    con-name "$connection_name" \
    ssid "$ssid"

/usr/bin/nmcli connection modify "$connection_name" \
    connection.autoconnect yes \
    connection.autoconnect-priority 100 \
    802-11-wireless.mode ap \
    802-11-wireless.band bg \
    802-11-wireless.channel 6 \
    802-11-wireless.powersave 2 \
    wifi-sec.key-mgmt wpa-psk \
    wifi-sec.psk "$ap_password" \
    ipv4.method shared \
    ipv4.addresses 192.168.4.1/24 \
    ipv6.method disabled

/usr/bin/nmcli connection modify "$development_connection" \
    connection.autoconnect no

trap - EXIT HUP INT TERM
unset ap_password

echo "SLOT-GUARD AP 프로필 생성 완료(다음 부팅 기본 운영 모드)"
echo "SSID: $ssid"
echo "주소: http://192.168.4.1:5000"
