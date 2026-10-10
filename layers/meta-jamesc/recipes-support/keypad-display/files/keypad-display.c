#include <errno.h>
#include <gpiod.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GPIO_CHIP "/dev/gpiochip0"

#define LCD_RS 25
#define LCD_ENABLE 24
#define LCD_D4 23
#define LCD_D5 17
#define LCD_D6 18
#define LCD_D7 22

#define ROW_COUNT 4U
#define COLUMN_COUNT 4U
#define DEBOUNCE_SCANS 3U
#define SCAN_INTERVAL_NS 10000000L
#define ROW_SETTLE_NS 50000L

static const unsigned int row_offsets[ROW_COUNT] = { 5, 6, 13, 19 };
static const unsigned int column_offsets[COLUMN_COUNT] = { 12, 16, 20, 21 };
static const char keymap[ROW_COUNT][COLUMN_COUNT] = {
    { '1', '2', '3', 'A' },
    { '4', '5', '6', 'B' },
    { '7', '8', '9', 'C' },
    { '*', '0', '#', 'D' },
};

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

static int lcd_show_text(struct gpiod_line_request *request, const char *text)
{
    size_t index;

    if (lcd_command(request, 0x01) < 0)
        return -1;

    for (index = 0; text[index] != '\0' && index < 16U; index++) {
        if (lcd_byte(request, (unsigned char)text[index], 1) < 0)
            return -1;
    }

    return 0;
}

static struct gpiod_line_request *request_lines(
    struct gpiod_chip *chip, const unsigned int *offsets, size_t count,
    enum gpiod_line_direction direction, enum gpiod_line_value output_value,
    enum gpiod_line_bias bias, const char *consumer)
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
        gpiod_line_settings_set_output_value(settings, output_value) < 0)
        goto out;
    if (direction == GPIOD_LINE_DIRECTION_INPUT &&
        gpiod_line_settings_set_bias(settings, bias) < 0)
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

static int scan_keypad(struct gpiod_line_request *rows,
                       struct gpiod_line_request *columns, uint16_t *mask)
{
    unsigned int row;
    unsigned int column;

    *mask = 0;
    for (row = 0; row < ROW_COUNT; row++) {
        if (set_line(rows, row_offsets[row], 0) < 0 ||
            sleep_ns(ROW_SETTLE_NS) < 0)
            return -1;

        for (column = 0; column < COLUMN_COUNT; column++) {
            enum gpiod_line_value value =
                gpiod_line_request_get_value(columns, column_offsets[column]);

            if (value == GPIOD_LINE_VALUE_ERROR)
                return -1;
            if (value == GPIOD_LINE_VALUE_INACTIVE)
                *mask |= (uint16_t)(1U << (row * COLUMN_COUNT + column));
        }

        if (set_line(rows, row_offsets[row], 1) < 0)
            return -1;
    }

    return 0;
}

static int show_new_keys(struct gpiod_line_request *lcd, uint16_t new_keys)
{
    unsigned int index;

    for (index = 0; index < ROW_COUNT * COLUMN_COUNT; index++) {
        char message[] = "KEY: X";
        char key;

        if ((new_keys & (uint16_t)(1U << index)) == 0)
            continue;

        key = keymap[index / COLUMN_COUNT][index % COLUMN_COUNT];
        message[5] = key;
        if (lcd_show_text(lcd, message) < 0)
            return -1;
    }

    return 0;
}

int main(void)
{
    static const unsigned int lcd_offsets[] = {
        LCD_RS, LCD_ENABLE, LCD_D4, LCD_D5, LCD_D6, LCD_D7,
    };
    struct gpiod_line_request *lcd_request = NULL;
    struct gpiod_line_request *row_request = NULL;
    struct gpiod_line_request *column_request = NULL;
    struct gpiod_chip *chip = NULL;
    uint16_t candidate_mask = 0;
    uint16_t stable_mask = 0;
    unsigned int candidate_scans = 0;
    int result = EXIT_FAILURE;

    signal(SIGINT, stop_running);
    signal(SIGTERM, stop_running);

    chip = gpiod_chip_open(GPIO_CHIP);
    if (!chip) {
        fprintf(stderr, "keypad-display: cannot open %s: %s\n",
                GPIO_CHIP, strerror(errno));
        goto out;
    }

    lcd_request = request_lines(
        chip, lcd_offsets, sizeof(lcd_offsets) / sizeof(lcd_offsets[0]),
        GPIOD_LINE_DIRECTION_OUTPUT, GPIOD_LINE_VALUE_INACTIVE,
        GPIOD_LINE_BIAS_AS_IS, "keypad-display-lcd");
    if (!lcd_request) {
        fprintf(stderr, "keypad-display: cannot request LCD GPIOs: %s\n",
                strerror(errno));
        goto out;
    }

    row_request = request_lines(
        chip, row_offsets, ROW_COUNT, GPIOD_LINE_DIRECTION_OUTPUT,
        GPIOD_LINE_VALUE_ACTIVE, GPIOD_LINE_BIAS_AS_IS, "keypad-display-rows");
    if (!row_request) {
        fprintf(stderr, "keypad-display: cannot request keypad rows: %s\n",
                strerror(errno));
        goto out;
    }

    column_request = request_lines(
        chip, column_offsets, COLUMN_COUNT, GPIOD_LINE_DIRECTION_INPUT,
        GPIOD_LINE_VALUE_INACTIVE, GPIOD_LINE_BIAS_PULL_UP,
        "keypad-display-columns");
    if (!column_request) {
        fprintf(stderr, "keypad-display: cannot request keypad columns: %s\n",
                strerror(errno));
        goto out;
    }

    if (lcd_init(lcd_request) < 0 ||
        lcd_show_text(lcd_request, "KEYPAD READY") < 0) {
        fprintf(stderr, "keypad-display: LCD initialization failed: %s\n",
                strerror(errno));
        goto out;
    }

    while (running) {
        uint16_t raw_mask;

        if (scan_keypad(row_request, column_request, &raw_mask) < 0) {
            fprintf(stderr, "keypad-display: keypad scan failed: %s\n",
                    strerror(errno));
            goto out;
        }

        if (raw_mask != candidate_mask) {
            candidate_mask = raw_mask;
            candidate_scans = 1;
        } else if (candidate_scans < DEBOUNCE_SCANS) {
            candidate_scans++;
        }

        if (candidate_scans == DEBOUNCE_SCANS &&
            candidate_mask != stable_mask) {
            uint16_t new_keys = candidate_mask & (uint16_t)~stable_mask;

            stable_mask = candidate_mask;
            if (show_new_keys(lcd_request, new_keys) < 0) {
                fprintf(stderr, "keypad-display: LCD write failed: %s\n",
                        strerror(errno));
                goto out;
            }
        }

        if (sleep_ns(SCAN_INTERVAL_NS) < 0) {
            fprintf(stderr, "keypad-display: sleep failed: %s\n",
                    strerror(errno));
            goto out;
        }
    }

    (void)lcd_command(lcd_request, 0x01);
    result = EXIT_SUCCESS;

out:
    gpiod_line_request_release(column_request);
    gpiod_line_request_release(row_request);
    gpiod_line_request_release(lcd_request);
    gpiod_chip_close(chip);
    return result;
}
