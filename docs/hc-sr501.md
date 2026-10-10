# HC-SR501 motion detector with HD44780 display

The project image runs `motion-display.service`. It reads an HC-SR501 PIR
sensor on GPIO27 and controls the project's parallel HD44780 LCD. When the
sensor output is high, the LCD shows `OBJECT DETECTED`. When it is low, the
LCD is blank.

## Wiring

Power off the Raspberry Pi before changing the wiring.

| HC-SR501 pin | Raspberry Pi connection | Physical pin |
| --- | --- | --- |
| VCC | 5 V | 2 or 4 |
| OUT | GPIO27 | 13 |
| GND | Ground | 6, 9, 14, 20, 25, 30, 34, or 39 |

Use the LCD wiring in [HD44780 character LCD](hd44780.md). The HC-SR501 OUT
signal is approximately 3.3 V and can connect directly to the Pi GPIO input.
Do not apply 5 V to a Pi GPIO.

The display keeps physical pins 11, 12, 15, 16, 18, and 22. The PIR uses
physical pin 13 for OUT, so it does not share any of the display's GPIO pins.

The sensor may take up to about one minute to stabilize after power-on and can
produce detections during that time. Its delay potentiometer controls how long
OUT remains high after motion. For the most direct response, start with the
delay near its minimum setting. Either trigger-mode jumper position works; the
retriggering position keeps the message visible while motion continues.

## Verify operation

After booting the image, check the service:

```sh
systemctl status motion-display --no-pager -l
journalctl -u motion-display -b --no-pager
```

A blank LCD is the normal idle state. Move in front of the sensor after its
power-on stabilization period; the LCD shows `OBJECT DETECTED` only while the
HC-SR501 OUT signal is high, then clears when OUT returns low.

The service owns GPIO17, GPIO18, GPIO22–GPIO25, and GPIO27. Confirm ownership
with:

```sh
gpioinfo -c gpiochip0 | grep -E 'line +(17|18|22|23|24|25|27):'
```

To run the standalone LCD test, stop the monitor first because both programs
use the same LCD GPIOs:

```sh
systemctl stop motion-display
/tmp/test-hd44780-rpi4b.sh
systemctl start motion-display
```

If motion does not produce a message, stop the service and observe the sensor
input independently:

```sh
systemctl stop motion-display
while true; do
    gpioget -c gpiochip0 27
    sleep 0.2
done
```

After the sensor has stabilized, its value should change from inactive to
active when motion is detected. Press Ctrl+C and restart the service when the
test is complete. If the input changes but the standalone LCD test displays
nothing, check the LCD wiring and contrast adjustment. If the input never
changes, check the HC-SR501 power, ground, GPIO27 connection, sensitivity, and
delay controls.

GPIO25 is shared with the MFRC522 reset line in the current project wiring, so
the motion display service and `mfrc522-tool` cannot be used at the same time.
