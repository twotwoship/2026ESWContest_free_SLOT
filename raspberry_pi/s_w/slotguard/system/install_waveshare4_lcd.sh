#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "root 권한으로 실행하십시오." >&2
    exit 1
fi

config_file=/boot/firmware/config.txt
overlay_target=/boot/firmware/overlays/waveshare35a.dtbo
overlay_url=https://raw.githubusercontent.com/waveshareteam/LCD-show/e91282e64dd1c37a009ebbd33967cf0b4ff6dce2/waveshare35a-overlay.dtb
overlay_sha256=4e879f464dd97387282e68de9ab66b9cc85627fb0b02618ca44cc0a94644ee7d
download_file=$(mktemp)
config_temp=$(mktemp)
backup_dir=/boot/firmware/slotguard-backup-before-lcd

trap 'rm -f "$download_file" "$config_temp"' EXIT

if [ ! -f "$config_file" ]; then
    echo "부팅 설정을 찾을 수 없습니다: $config_file" >&2
    exit 3
fi

mkdir -p "$backup_dir"
if [ ! -f "$backup_dir/config.txt" ]; then
    cp "$config_file" "$backup_dir/config.txt"
fi
if [ -f "$overlay_target" ] && [ ! -f "$backup_dir/waveshare35a.dtbo" ]; then
    cp "$overlay_target" "$backup_dir/waveshare35a.dtbo"
fi

curl --fail --location --silent --show-error \
    "$overlay_url" --output "$download_file"
printf '%s  %s\n' "$overlay_sha256" "$download_file" | sha256sum -c -
install -o root -g root -m 0644 "$download_file" "$overlay_target"

sed '/# BEGIN SLOT-GUARD WAVESHARE4/,/# END SLOT-GUARD WAVESHARE4/d' \
    "$config_file" > "$config_temp"
printf '%s\n' \
    '' \
    '# BEGIN SLOT-GUARD WAVESHARE4' \
    '# Waveshare 4inch RPi LCD (A) Rev 2.0, 480x320 landscape' \
    'dtparam=spi=on' \
    'enable_uart=1' \
    'dtoverlay=waveshare35a,rotate=90' \
    '# END SLOT-GUARD WAVESHARE4' >> "$config_temp"
install -o root -g root -m 0644 "$config_temp" "$config_file"

echo "Waveshare 4inch LCD 오버레이 설치 완료"
echo "백업: $backup_dir/config.txt"
echo "재부팅 후 /dev/fb1 및 ADS7846 Touchscreen을 확인하십시오."
