#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "root 권한으로 실행하십시오." >&2
    exit 1
fi

if [ "$#" -ne 1 ]; then
    echo "사용법: sudo ./system/install_slotguard.sh <자동로그인사용자>" >&2
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
app_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
app_group=$(id -gn "$app_user")
service_temp=$(mktemp)
sudoers_temp=$(mktemp)
lightdm_temp=$(mktemp)

trap 'rm -f "$service_temp" "$sudoers_temp" "$lightdm_temp"' EXIT

if [ ! -x "$app_dir/.venv/bin/python" ]; then
    echo "프로젝트 .venv를 찾을 수 없습니다: $app_dir/.venv" >&2
    exit 3
fi

if ! dpkg-query -W -f='${Status}' xserver-xorg-video-fbdev 2>/dev/null \
        | grep -q 'install ok installed' \
        || ! dpkg-query -W -f='${Status}' xserver-xorg-input-evdev \
        2>/dev/null | grep -q 'install ok installed'; then
    apt-get update
    apt-get install -y xserver-xorg-video-fbdev xserver-xorg-input-evdev
fi

if ! dpkg-query -W -f='${Status}' fonts-noto-cjk 2>/dev/null \
        | grep -q 'install ok installed'; then
    apt-get update
    apt-get install -y fonts-noto-cjk
fi

usermod -a -G dialout,video,input "$app_user"

sed \
    -e "s|__APP_USER__|$app_user|g" \
    -e "s|__APP_GROUP__|$app_group|g" \
    -e "s|__APP_DIR__|$app_dir|g" \
    "$script_dir/slotguard.service.in" > "$service_temp"

install -o root -g root -m 0644 \
    "$service_temp" /etc/systemd/system/slotguard.service
install -o root -g root -m 0755 \
    "$script_dir/slotguard-power" /usr/local/sbin/slotguard-power
install -o root -g root -m 0755 \
    "$script_dir/slotguard-xsession" /usr/local/bin/slotguard-xsession
install -o root -g root -m 0644 \
    "$script_dir/slotguard.desktop" /usr/share/xsessions/slotguard.desktop
install -d -o root -g root -m 0755 /etc/chromium/policies
install -d -o root -g root -m 0755 /etc/chromium/policies/managed
install -o root -g root -m 0644 \
    "$script_dir/slotguard-chromium-policy.json" \
    /etc/chromium/policies/managed/slotguard-kiosk.json
mkdir -p /etc/X11/xorg.conf.d
install -o root -g root -m 0644 \
    "$script_dir/90-slotguard-lcd.conf" \
    /etc/X11/xorg.conf.d/90-slotguard-lcd.conf

printf '%s ALL=(root) NOPASSWD: /usr/local/sbin/slotguard-power poweroff, /usr/local/sbin/slotguard-power reboot\n' \
    "$app_user" > "$sudoers_temp"
chmod 0440 "$sudoers_temp"
visudo -cf "$sudoers_temp"
install -o root -g root -m 0440 \
    "$sudoers_temp" /etc/sudoers.d/slotguard-power

mkdir -p /etc/lightdm/lightdm.conf.d
printf '%s\n' \
    '[Seat:*]' \
    "autologin-user=$app_user" \
    'autologin-session=slotguard' \
    'user-session=slotguard' > "$lightdm_temp"
install -o root -g root -m 0644 \
    "$lightdm_temp" /etc/lightdm/lightdm.conf.d/90-slotguard.conf

# Raspberry Pi OS writes active session values directly into lightdm.conf.
# lightdm.conf is loaded after lightdm.conf.d and would otherwise override the
# SLOT-GUARD kiosk session with the default rpd-labwc desktop.
if [ -f /etc/lightdm/lightdm.conf ]; then
    if [ ! -e /etc/lightdm/lightdm.conf.slotguard-before-kiosk ]; then
        cp -a /etc/lightdm/lightdm.conf \
            /etc/lightdm/lightdm.conf.slotguard-before-kiosk
    fi
    sed -i \
        -e 's/^[[:space:]]*autologin-session=.*/autologin-session=slotguard/' \
        -e 's/^[[:space:]]*user-session=.*/user-session=slotguard/' \
        /etc/lightdm/lightdm.conf
fi

systemctl daemon-reload
systemctl enable slotguard.service
"$script_dir/install_time_helper.sh" "$app_user"

echo "SLOT-GUARD 서비스·키오스크·전원 도우미 설치 완료"
echo "LCD 드라이버 설치 후 재부팅하십시오."
