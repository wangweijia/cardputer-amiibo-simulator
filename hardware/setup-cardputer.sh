#!/bin/sh
# Run explicitly as root on Cardputer Zero. Reboot after successful setup.
set -eu
test "$(id -u)" = 0 || { echo 'Run as root.' >&2; exit 1; }
source_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
config=/boot/firmware/config.txt
test -f /boot/firmware/overlays/spi0-spidev2-gpio22-overlay.dtbo
test -e /sys/class/leds/ext_5v_out/brightness
test -e /sys/class/leds/ext_usb_gpio_fun/brightness
backup_dir=$(mktemp -d /var/lib/amiibozero-hardware-XXXXXXXX)
cp -p "$config" "$backup_dir/config.txt"
for previous in /usr/local/libexec/amiibozero-cap-power /etc/systemd/system/amiibozero-cap-power.service; do
    if test -f "$previous"; then cp -p "$previous" "$backup_dir/"; fi
done
if ! grep -q '^dtoverlay=spi0-spidev2-gpio22-overlay$' "$config"; then
    printf '\n[all]\n# AmiiboZero: persistent Cap NFC CS2 device\ndtoverlay=spi0-spidev2-gpio22-overlay\n' >> "$config"
fi
install -d -m 755 /usr/local/libexec
install -m 755 "$source_dir/cap-power.sh" /usr/local/libexec/amiibozero-cap-power
install -m 644 "$source_dir/amiibozero-cap-power.service" /etc/systemd/system/amiibozero-cap-power.service
systemctl daemon-reload
systemctl enable --now amiibozero-cap-power.service
echo "Backup: $backup_dir"
echo 'Reboot to apply the persistent NFC SPI overlay.'
