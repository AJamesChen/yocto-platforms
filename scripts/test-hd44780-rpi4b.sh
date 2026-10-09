#!/bin/sh
set -eu

hold_period=${1:-30s}

case ${hold_period} in
    *[!0-9a-zA-Z]*)
        echo "error: hold period must use gpioset syntax, for example 30s" >&2
        exit 2
        ;;
esac

command -v gpioset >/dev/null 2>&1 || {
    echo "error: gpioset is not installed" >&2
    exit 1
}

# Verified Raspberry Pi 4B BCM GPIO wiring for a bare HD44780 in 4-bit mode.
rs=25
enable=24
d4=23
d5=17
d6=18
d7=22

lcd_nibble() {
    value=$1
    register_select=$2

    printf 'set %s=%s %s=%s %s=%s %s=%s %s=%s %s=0\n' \
        "${rs}" "${register_select}" \
        "${d4}" "$((value & 1))" \
        "${d5}" "$(((value >> 1) & 1))" \
        "${d6}" "$(((value >> 2) & 1))" \
        "${d7}" "$(((value >> 3) & 1))" \
        "${enable}"

    printf 'set %s=1\nsleep 1us\nset %s=0\nsleep 50us\n' \
        "${enable}" "${enable}"
}

lcd_byte() {
    value=$1
    register_select=$2

    lcd_nibble "$(((value >> 4) & 15))" "${register_select}"
    lcd_nibble "$((value & 15))" "${register_select}"
}

{
    echo 'sleep 50ms'

    # HD44780 power-on initialization into 4-bit mode.
    lcd_nibble 3 0
    echo 'sleep 5ms'
    lcd_nibble 3 0
    echo 'sleep 200us'
    lcd_nibble 3 0
    echo 'sleep 200us'
    lcd_nibble 2 0

    # Two lines, display off, clear, increment cursor, display on.
    lcd_byte 0x28 0
    lcd_byte 0x08 0
    lcd_byte 0x01 0
    echo 'sleep 2ms'
    lcd_byte 0x06 0
    lcd_byte 0x0c 0

    # Write "LCD TEST OK".
    for value in \
        0x4c 0x43 0x44 0x20 0x54 0x45 0x53 0x54 0x20 0x4f 0x4b
    do
        lcd_byte "${value}" 1
    done

    echo "sleep ${hold_period}"
    echo 'exit'
} | gpioset -i -c gpiochip0 -C hd44780 \
    25=0 24=0 23=0 17=0 18=0 22=0
