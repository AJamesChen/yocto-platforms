#include <errno.h>
#include <gpiod.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GPIO_CHIP "/dev/gpiochip0"
#define PIR_GPIO 27

#define LCD_RS 25
#define LCD_ENABLE 24
#define LCD_D4 23
#define LCD_D5 17
#define LCD_D6 18
#define LCD_D7 22

static volatile sig_atomic_t running = 1;

static void stop_running(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static int sleep_ns(long nanoseconds)
{
    struct timespec delay = {
        .tv_sec = nanoseconds / 1000000000L,
        .tv_nsec = nanoseconds % 1000000000L,
    };

    while (nanosleep(&delay, &delay) < 0) {
        if (errno != EINTR)
            return -1;
        if (!running)
            break;
    }

    return 0;
}

static int set_line(struct gpiod_line_request *request,
                    unsigned int offset, int value)
{
    return gpiod_line_request_set_value(
        request, offset,
        value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
}

static int lcd_nibble(struct gpiod_line_request *request,
                      unsigned int value, int register_select)
{
    if (set_line(request, LCD_RS, register_select) < 0 ||
        set_line(request, LCD_D4, value & 1U) < 0 ||
        set_line(request, LCD_D5, (value >> 1) & 1U) < 0 ||
        set_line(request, LCD_D6, (value >> 2) & 1U) < 0 ||
        set_line(request, LCD_D7, (value >> 3) & 1U) < 0 ||
        set_line(request, LCD_ENABLE, 1) < 0 ||
        sleep_ns(1000L) < 0 ||
        set_line(request, LCD_ENABLE, 0) < 0 ||
        sleep_ns(50000L) < 0)
        return -1;

    return 0;
}

static int lcd_byte(struct gpiod_line_request *request,
                    unsigned int value, int register_select)
{
    if (lcd_nibble(request, (value >> 4) & 0x0fU, register_select) < 0 ||
        lcd_nibble(request, value & 0x0fU, register_select) < 0)
        return -1;

    return 0;
}

static int lcd_command(struct gpiod_line_request *request, unsigned int command)
{
    if (lcd_byte(request, command, 0) < 0)
        return -1;

    if (command == 0x01U || command == 0x02U)
        return sleep_ns(2000000L);

    return 0;
}

static int lcd_init(struct gpiod_line_request *request)
{
    if (sleep_ns(50000000L) < 0 ||
        lcd_nibble(request, 3, 0) < 0 ||
        sleep_ns(5000000L) < 0 ||
        lcd_nibble(request, 3, 0) < 0 ||
        sleep_ns(200000L) < 0 ||
        lcd_nibble(request, 3, 0) < 0 ||
        sleep_ns(200000L) < 0 ||
        lcd_nibble(request, 2, 0) < 0 ||
        lcd_command(request, 0x28) < 0 ||
        lcd_command(request, 0x08) < 0 ||
        lcd_command(request, 0x01) < 0 ||
        lcd_command(request, 0x06) < 0 ||
        lcd_command(request, 0x0c) < 0)
        return -1;

    return 0;
}

static int lcd_show_detected(struct gpiod_line_request *request)
{
    static const char message[] = "OBJECT DETECTED";
    size_t index;

    if (lcd_command(request, 0x01) < 0)
        return -1;

    for (index = 0; index < sizeof(message) - 1; index++) {
        if (lcd_byte(request, (unsigned char)message[index], 1) < 0)
            return -1;
    }

    return 0;
}

static struct gpiod_line_request *request_lines(
    struct gpiod_chip *chip, const unsigned int *offsets, size_t count,
    enum gpiod_line_direction direction, const char *consumer)
{
    struct gpiod_request_config *request_config = NULL;
    struct gpiod_line_config *line_config = NULL;
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_request *request = NULL;

    settings = gpiod_line_settings_new();
    line_config = gpiod_line_config_new();
    request_config = gpiod_request_config_new();
    if (!settings || !line_config || !request_config)
        goto out;

    if (gpiod_line_settings_set_direction(settings, direction) < 0)
        goto out;
    if (direction == GPIOD_LINE_DIRECTION_OUTPUT &&
        gpiod_line_settings_set_output_value(
            settings, GPIOD_LINE_VALUE_INACTIVE) < 0)
        goto out;
    if (gpiod_line_config_add_line_settings(
            line_config, offsets, count, settings) < 0)
        goto out;

    gpiod_request_config_set_consumer(request_config, consumer);
    request = gpiod_chip_request_lines(chip, request_config, line_config);

out:
    gpiod_request_config_free(request_config);
    gpiod_line_config_free(line_config);
    gpiod_line_settings_free(settings);
    return request;
}

int main(void)
{
    static const unsigned int lcd_offsets[] = {
        LCD_RS, LCD_ENABLE, LCD_D4, LCD_D5, LCD_D6, LCD_D7,
    };
    static const unsigned int pir_offsets[] = { PIR_GPIO };
    struct gpiod_line_request *lcd_request = NULL;
    struct gpiod_line_request *pir_request = NULL;
    struct gpiod_chip *chip = NULL;
    int previous = -1;
    int result = EXIT_FAILURE;

    signal(SIGINT, stop_running);
    signal(SIGTERM, stop_running);

    chip = gpiod_chip_open(GPIO_CHIP);
    if (!chip) {
        fprintf(stderr, "motion-display: cannot open %s: %s\n",
                GPIO_CHIP, strerror(errno));
        goto out;
    }

    lcd_request = request_lines(chip, lcd_offsets,
                                sizeof(lcd_offsets) / sizeof(lcd_offsets[0]),
                                GPIOD_LINE_DIRECTION_OUTPUT, "motion-display-lcd");
    if (!lcd_request) {
        fprintf(stderr, "motion-display: cannot request LCD GPIOs: %s\n",
                strerror(errno));
        goto out;
    }

    pir_request = request_lines(chip, pir_offsets, 1,
                                GPIOD_LINE_DIRECTION_INPUT, "motion-display-pir");
    if (!pir_request) {
        fprintf(stderr, "motion-display: cannot request PIR GPIO%d: %s\n",
                PIR_GPIO, strerror(errno));
        goto out;
    }

    if (lcd_init(lcd_request) < 0) {
        fprintf(stderr, "motion-display: LCD initialization failed: %s\n",
                strerror(errno));
        goto out;
    }

    while (running) {
        enum gpiod_line_value value =
            gpiod_line_request_get_value(pir_request, PIR_GPIO);

        if (value == GPIOD_LINE_VALUE_ERROR) {
            fprintf(stderr, "motion-display: PIR read failed: %s\n",
                    strerror(errno));
            goto out;
        }

        if ((int)value != previous) {
            if (value == GPIOD_LINE_VALUE_ACTIVE) {
                if (lcd_show_detected(lcd_request) < 0)
                    goto lcd_error;
            } else if (lcd_command(lcd_request, 0x01) < 0) {
                goto lcd_error;
            }
            previous = (int)value;
        }

        if (sleep_ns(100000000L) < 0) {
            fprintf(stderr, "motion-display: sleep failed: %s\n",
                    strerror(errno));
            goto out;
        }
    }

    (void)lcd_command(lcd_request, 0x01);
    result = EXIT_SUCCESS;
    goto out;

lcd_error:
    fprintf(stderr, "motion-display: LCD write failed: %s\n", strerror(errno));
out:
    gpiod_line_request_release(pir_request);
    gpiod_line_request_release(lcd_request);
    gpiod_chip_close(chip);
    return result;
}
