#!/bin/sh
# Cardputer Zero BSP interfaces; keep shared LCD/Cap SPI power enabled.
set -eu
printf '0' > /sys/class/leds/ext_usb_gpio_fun/brightness
/usr/bin/pinctrl set 26 op dh
printf '1' > /sys/class/leds/ext_5v_out/brightness
sleep 0.1
test "$(cat /sys/class/leds/ext_usb_gpio_fun/brightness)" = 0
test "$(cat /sys/class/leds/ext_5v_out/brightness)" -gt 0
