#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "root 권한으로 실행하십시오." >&2
    exit 1
fi

if [ "$#" -ne 1 ]; then
    echo "사용법: sudo ./system/install_time_helper.sh <Flask실행사용자>" >&2
    exit 2
fi

app_user=$1

case "$app_user" in
    *[!A-Za-z0-9_-]*|'')
        echo "올바르지 않은 사용자 이름입니다." >&2
        exit 2
        ;;
esac

if ! getent passwd "$app_user" >/dev/null; then
    echo "존재하지 않는 사용자입니다: $app_user" >&2
    exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
helper_source="$script_dir/slotguard-set-time"
helper_target=/usr/local/sbin/slotguard-set-time
sudoers_target=/etc/sudoers.d/slotguard-set-time
sudoers_temp=$(mktemp)

trap 'rm -f "$sudoers_temp"' EXIT

install -o root -g root -m 0755 "$helper_source" "$helper_target"

printf '%s ALL=(root) NOPASSWD: %s *\n' \
    "$app_user" \
    "$helper_target" \
    > "$sudoers_temp"

chmod 0440 "$sudoers_temp"
visudo -cf "$sudoers_temp"
install -o root -g root -m 0440 "$sudoers_temp" "$sudoers_target"

timedatectl set-timezone Asia/Seoul
timedatectl set-ntp false

echo "SLOT-GUARD 시간 도우미 설치 완료"
