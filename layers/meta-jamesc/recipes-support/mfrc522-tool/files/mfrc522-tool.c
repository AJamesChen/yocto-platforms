// SPDX-License-Identifier: MIT
// MFRC522 SPI utility for Raspberry Pi 4B.

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

enum {
  CommandReg = 0x01,
  CommIEnReg = 0x02,
  CommIrqReg = 0x04,
  DivIrqReg = 0x05,
  ErrorReg = 0x06,
  Status2Reg = 0x08,
  FIFODataReg = 0x09,
  FIFOLevelReg = 0x0a,
  ControlReg = 0x0c,
  BitFramingReg = 0x0d,
  ModeReg = 0x11,
  TxControlReg = 0x14,
  TxASKReg = 0x15,
  CRCResultRegH = 0x21,
  CRCResultRegL = 0x22,
  TModeReg = 0x2a,
  TPrescalerReg = 0x2b,
  TReloadRegH = 0x2c,
  TReloadRegL = 0x2d,
  VersionReg = 0x37,
};

enum {
  PCD_Idle = 0x00,
  PCD_CalcCRC = 0x03,
  PCD_Transceive = 0x0c,
  PCD_MFAuthent = 0x0e,
  PCD_SoftReset = 0x0f,
};

enum {
  PICC_REQA = 0x26,
  PICC_SEL_CL1 = 0x93,
  PICC_SEL_CL2 = 0x95,
  PICC_SEL_CL3 = 0x97,
  PICC_CT = 0x88,
  PICC_MF_AUTH_KEY_A = 0x60,
  PICC_MF_AUTH_KEY_B = 0x61,
  PICC_MF_READ = 0x30,
  PICC_MF_WRITE = 0xa0,
};

struct device {
  int spi_fd;
  int gpio_fd;
  uint32_t speed_hz;
};

struct card {
  uint8_t uid[10];
  size_t uid_len;
  uint8_t sak;
  uint8_t atqa[2];
};

static void sleep_us(long usec) {
  struct timespec delay = {
      .tv_sec = usec / 1000000,
      .tv_nsec = (usec % 1000000) * 1000,
  };
  while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
    ;
}

static int spi_transfer(struct device *dev, const uint8_t *tx, uint8_t *rx,
                        size_t length) {
  struct spi_ioc_transfer transfer = {
      .tx_buf = (uintptr_t)tx,
      .rx_buf = (uintptr_t)rx,
      .len = length,
      .speed_hz = dev->speed_hz,
      .bits_per_word = 8,
  };
  if (ioctl(dev->spi_fd, SPI_IOC_MESSAGE(1), &transfer) < 0) {
    perror("SPI transfer");
    return -1;
  }
  return 0;
}

static int write_reg(struct device *dev, uint8_t reg, uint8_t value) {
  uint8_t tx[2] = {(uint8_t)((reg << 1) & 0x7e), value};
  return spi_transfer(dev, tx, NULL, sizeof(tx));
}

static int read_reg(struct device *dev, uint8_t reg, uint8_t *value) {
  uint8_t tx[2] = {(uint8_t)(0x80 | ((reg << 1) & 0x7e)), 0};
  uint8_t rx[2] = {0};
  if (spi_transfer(dev, tx, rx, sizeof(tx)) != 0)
    return -1;
  *value = rx[1];
  return 0;
}

static int set_bits(struct device *dev, uint8_t reg, uint8_t mask) {
  uint8_t value;
  return read_reg(dev, reg, &value) == 0 ? write_reg(dev, reg, value | mask)
                                         : -1;
}

static int clear_bits(struct device *dev, uint8_t reg, uint8_t mask) {
  uint8_t value;
  return read_reg(dev, reg, &value) == 0
             ? write_reg(dev, reg, value & (uint8_t)~mask)
             : -1;
}

static int reset_gpio(const char *chip_path, unsigned int line) {
  struct gpiohandle_request request = {
      .lines = 1,
      .flags = GPIOHANDLE_REQUEST_OUTPUT,
      .default_values = {1},
  };
  struct gpiohandle_data data = {.values = {0}};
  int chip_fd;

  request.lineoffsets[0] = line;
  snprintf(request.consumer_label, sizeof(request.consumer_label),
           "mfrc522-reset");
  chip_fd = open(chip_path, O_RDONLY | O_CLOEXEC);
  if (chip_fd < 0)
    return -1;
  if (ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &request) < 0) {
    close(chip_fd);
    return -1;
  }
  close(chip_fd);
  if (ioctl(request.fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0) {
    close(request.fd);
    return -1;
  }
  sleep_us(5000);
  data.values[0] = 1;
  if (ioctl(request.fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0) {
    close(request.fd);
    return -1;
  }
  sleep_us(50000);
  return request.fd;
}

static int open_device(struct device *dev, const char *spi_path,
                       const char *gpiochip, unsigned int reset_line,
                       uint32_t speed_hz) {
  uint8_t mode = SPI_MODE_0;
  uint8_t bits = 8;
  uint8_t command;
  uint8_t version;

  *dev = (struct device){.spi_fd = -1, .gpio_fd = -1, .speed_hz = speed_hz};
  dev->gpio_fd = reset_gpio(gpiochip, reset_line);
  if (dev->gpio_fd < 0) {
    fprintf(stderr, "cannot reset MFRC522 using %s line %u: %s\n", gpiochip,
            reset_line, strerror(errno));
    return -1;
  }
  dev->spi_fd = open(spi_path, O_RDWR | O_CLOEXEC);
  if (dev->spi_fd < 0) {
    fprintf(stderr, "cannot open %s: %s\n", spi_path, strerror(errno));
    return -1;
  }
  if (ioctl(dev->spi_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
      ioctl(dev->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
      ioctl(dev->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz) < 0) {
    perror("configure SPI");
    return -1;
  }
  if (read_reg(dev, VersionReg, &version) != 0)
    return -1;
  if (version == 0x00 || version == 0xff) {
    fprintf(stderr,
            "MFRC522 did not respond on %s: VersionReg=0x%02x; check 3.3V, "
            "SDA/CS, SCK, MOSI, MISO, and RST\n",
            spi_path, version);
    errno = ENODEV;
    return -1;
  }
  if (write_reg(dev, CommandReg, PCD_SoftReset) != 0)
    return -1;
  for (unsigned int attempt = 0; attempt < 100; ++attempt) {
    sleep_us(1000);
    if (read_reg(dev, CommandReg, &command) != 0)
      return -1;
    if ((command & 0x10) == 0)
      break;
    if (attempt == 99) {
      fprintf(stderr,
              "MFRC522 software reset timed out on %s: CommandReg=0x%02x\n",
              spi_path, command);
      errno = ETIMEDOUT;
      return -1;
    }
  }
  if (write_reg(dev, TModeReg, 0x80) != 0 ||
      write_reg(dev, TPrescalerReg, 0xa9) != 0 ||
      write_reg(dev, TReloadRegH, 0x03) != 0 ||
      write_reg(dev, TReloadRegL, 0xe8) != 0 ||
      write_reg(dev, TxASKReg, 0x40) != 0 || write_reg(dev, ModeReg, 0x3d) != 0)
    return -1;
  return set_bits(dev, TxControlReg, 0x03);
}

static void close_device(struct device *dev) {
  if (dev->spi_fd >= 0)
    close(dev->spi_fd);
  if (dev->gpio_fd >= 0)
    close(dev->gpio_fd);
}

static int calculate_crc(struct device *dev, const uint8_t *data, size_t length,
                         uint8_t result[2]) {
  uint8_t irq;
  if (write_reg(dev, CommandReg, PCD_Idle) != 0 ||
      write_reg(dev, DivIrqReg, 0x04) != 0 ||
      write_reg(dev, FIFOLevelReg, 0x80) != 0)
    return -1;
  for (size_t i = 0; i < length; ++i)
    if (write_reg(dev, FIFODataReg, data[i]) != 0)
      return -1;
  if (write_reg(dev, CommandReg, PCD_CalcCRC) != 0)
    return -1;
  for (unsigned int attempt = 0; attempt < 100; ++attempt) {
    if (read_reg(dev, DivIrqReg, &irq) != 0)
      return -1;
    if (irq & 0x04)
      break;
    sleep_us(1000);
    if (attempt == 99) {
      errno = ETIMEDOUT;
      return -1;
    }
  }
  return write_reg(dev, CommandReg, PCD_Idle) == 0 &&
                 read_reg(dev, CRCResultRegL, &result[0]) == 0 &&
                 read_reg(dev, CRCResultRegH, &result[1]) == 0
             ? 0
             : -1;
}

static int communicate(struct device *dev, uint8_t command, uint8_t wait_irq,
                       const uint8_t *send, size_t send_len, uint8_t *receive,
                       size_t capacity, size_t *receive_len,
                       uint8_t *valid_bits) {
  uint8_t irq = 0, error, level;
  if (write_reg(dev, CommandReg, PCD_Idle) != 0 ||
      write_reg(dev, CommIEnReg, wait_irq | 0x80) != 0 ||
      write_reg(dev, CommIrqReg, 0x7f) != 0 ||
      write_reg(dev, FIFOLevelReg, 0x80) != 0)
    return -1;
  for (size_t i = 0; i < send_len; ++i)
    if (write_reg(dev, FIFODataReg, send[i]) != 0)
      return -1;
  if (write_reg(dev, CommandReg, command) != 0)
    return -1;
  if (command == PCD_Transceive && set_bits(dev, BitFramingReg, 0x80) != 0)
    return -1;
  for (unsigned int attempt = 0; attempt < 2000; ++attempt) {
    if (read_reg(dev, CommIrqReg, &irq) != 0)
      return -1;
    if (irq & wait_irq)
      break;
    if (irq & 0x01) {
      errno = ETIMEDOUT;
      break;
    }
    sleep_us(100);
  }
  if (command == PCD_Transceive && clear_bits(dev, BitFramingReg, 0x80) != 0)
    return -1;
  if (!(irq & wait_irq)) {
    errno = ETIMEDOUT;
    return -1;
  }
  if (read_reg(dev, ErrorReg, &error) != 0)
    return -1;
  if (error & 0x13) {
    errno = EIO;
    return -1;
  }
  if (receive_len != NULL) {
    if (read_reg(dev, FIFOLevelReg, &level) != 0)
      return -1;
    if (level > capacity) {
      errno = ENOSPC;
      return -1;
    }
    for (uint8_t i = 0; i < level; ++i)
      if (read_reg(dev, FIFODataReg, &receive[i]) != 0)
        return -1;
    *receive_len = level;
    if (valid_bits != NULL) {
      if (read_reg(dev, ControlReg, valid_bits) != 0)
        return -1;
      *valid_bits &= 0x07;
    }
  }
  return 0;
}

static int transceive(struct device *dev, const uint8_t *send, size_t send_len,
                      uint8_t *receive, size_t capacity, size_t *receive_len,
                      uint8_t *valid_bits) {
  return communicate(dev, PCD_Transceive, 0x30, send, send_len, receive,
                     capacity, receive_len, valid_bits);
}

static int request_card(struct device *dev, struct card *card) {
  uint8_t request = PICC_REQA, bits = 0;
  size_t length = 0;
  if (write_reg(dev, BitFramingReg, 0x07) != 0 ||
      transceive(dev, &request, 1, card->atqa, sizeof(card->atqa), &length,
                 &bits) != 0)
    return -1;
  if (length != 2 || bits != 0) {
    errno = EPROTO;
    return -1;
  }
  return 0;
}

static int select_card(struct device *dev, struct card *card) {
  const uint8_t selects[] = {PICC_SEL_CL1, PICC_SEL_CL2, PICC_SEL_CL3};
  uint8_t response[10], frame[9], sak_crc[2];
  card->uid_len = 0;
  for (size_t level = 0; level < ARRAY_SIZE(selects); ++level) {
    uint8_t anticollision[2] = {selects[level], 0x20};
    uint8_t bits = 0;
    size_t length = 0;
    if (write_reg(dev, BitFramingReg, 0) != 0 ||
        transceive(dev, anticollision, 2, response, sizeof(response), &length,
                   &bits) != 0)
      return -1;
    if (length != 5 || bits != 0 ||
        (response[0] ^ response[1] ^ response[2] ^ response[3]) !=
            response[4]) {
      errno = EBADMSG;
      return -1;
    }
    frame[0] = selects[level];
    frame[1] = 0x70;
    memcpy(&frame[2], response, 5);
    if (calculate_crc(dev, frame, 7, &frame[7]) != 0)
      return -1;
    if (transceive(dev, frame, sizeof(frame), response, sizeof(response),
                   &length, &bits) != 0 ||
        length != 3 || bits != 0) {
      errno = EPROTO;
      return -1;
    }
    if (calculate_crc(dev, response, 1, sak_crc) != 0)
      return -1;
    if (sak_crc[0] != response[1] || sak_crc[1] != response[2]) {
      errno = EBADMSG;
      return -1;
    }
    card->sak = response[0];
    if (frame[2] == PICC_CT) {
      memcpy(&card->uid[card->uid_len], &frame[3], 3);
      card->uid_len += 3;
    } else {
      memcpy(&card->uid[card->uid_len], &frame[2], 4);
      card->uid_len += 4;
    }
    if (!(card->sak & 0x04))
      return 0;
  }
  errno = EPROTO;
  return -1;
}

static int detect_card(struct device *dev, struct card *card) {
  memset(card, 0, sizeof(*card));
  return request_card(dev, card) == 0 ? select_card(dev, card) : -1;
}

static int authenticate(struct device *dev, const struct card *card,
                        uint8_t block, uint8_t key_command,
                        const uint8_t key[6]) {
  uint8_t frame[12], status;
  frame[0] = key_command;
  frame[1] = block;
  memcpy(&frame[2], key, 6);
  memcpy(&frame[8], &card->uid[card->uid_len - 4], 4);
  if (communicate(dev, PCD_MFAuthent, 0x10, frame, sizeof(frame), NULL, 0, NULL,
                  NULL) != 0 ||
      read_reg(dev, Status2Reg, &status) != 0)
    return -1;
  if (!(status & 0x08)) {
    errno = EACCES;
    return -1;
  }
  return 0;
}

static int mifare_command(struct device *dev, uint8_t command, uint8_t block,
                          uint8_t *response, size_t capacity, size_t *length,
                          uint8_t *bits) {
  uint8_t frame[4] = {command, block};
  return calculate_crc(dev, frame, 2, &frame[2]) == 0
             ? transceive(dev, frame, sizeof(frame), response, capacity, length,
                          bits)
             : -1;
}

static int read_block(struct device *dev, uint8_t block, uint8_t data[16]) {
  uint8_t response[18], bits = 0, crc[2];
  size_t length = 0;
  if (mifare_command(dev, PICC_MF_READ, block, response, sizeof(response),
                     &length, &bits) != 0)
    return -1;
  if (length != sizeof(response) || bits != 0) {
    errno = EPROTO;
    return -1;
  }
  if (calculate_crc(dev, response, 16, crc) != 0)
    return -1;
  if (crc[0] != response[16] || crc[1] != response[17]) {
    errno = EBADMSG;
    return -1;
  }
  memcpy(data, response, 16);
  return 0;
}

static int expect_ack(struct device *dev, const uint8_t *frame,
                      size_t frame_len) {
  uint8_t response[2], bits = 0;
  size_t length = 0;
  if (transceive(dev, frame, frame_len, response, sizeof(response), &length,
                 &bits) != 0)
    return -1;
  if (length != 1 || bits != 4 || (response[0] & 0x0f) != 0x0a) {
    errno = EPROTO;
    return -1;
  }
  return 0;
}

static bool is_trailer_block(uint8_t block) {
  return block < 128 ? block % 4 == 3 : block % 16 == 15;
}

static int write_block(struct device *dev, uint8_t block,
                       const uint8_t data[16]) {
  uint8_t command[4] = {PICC_MF_WRITE, block};
  uint8_t frame[18];
  if (block == 0 || is_trailer_block(block)) {
    fprintf(stderr, "refusing manufacturer or sector-trailer block %u\n",
            block);
    errno = EPERM;
    return -1;
  }
  if (calculate_crc(dev, command, 2, &command[2]) != 0 ||
      expect_ack(dev, command, sizeof(command)) != 0)
    return -1;
  memcpy(frame, data, 16);
  if (calculate_crc(dev, frame, 16, &frame[16]) != 0)
    return -1;
  return expect_ack(dev, frame, sizeof(frame));
}

static int parse_hex(const char *text, uint8_t *output, size_t length) {
  if (strlen(text) != length * 2)
    return -1;
  for (size_t i = 0; i < length; ++i) {
    char byte[3] = {text[i * 2], text[i * 2 + 1], 0};
    char *end;
    unsigned long value = strtoul(byte, &end, 16);
    if (*end || value > 0xff)
      return -1;
    output[i] = (uint8_t)value;
  }
  return 0;
}

static int parse_block(const char *text, uint8_t *block) {
  char *end;
  unsigned long value = strtoul(text, &end, 0);
  if (!*text || *end || value > 255)
    return -1;
  *block = (uint8_t)value;
  return 0;
}

static void print_hex(const uint8_t *data, size_t length) {
  for (size_t i = 0; i < length; ++i)
    printf("%02x", data[i]);
}

static void print_card(const struct card *card) {
  printf("uid=");
  print_hex(card->uid, card->uid_len);
  printf(" atqa=%02x%02x sak=%02x\n", card->atqa[0], card->atqa[1], card->sak);
}

static void usage(FILE *stream, const char *program) {
  fprintf(stream,
          "usage: %s [options] version\n"
          "       %s [options] scan\n"
          "       %s [options] read BLOCK A|B KEY_HEX\n"
          "       %s [options] write BLOCK A|B KEY_HEX DATA_HEX\n\n"
          "options:\n"
          "  -d, --device PATH       SPI device (default /dev/spidev0.0)\n"
          "  -s, --speed HZ          SPI speed (default 1000000)\n"
          "  -g, --gpiochip PATH     GPIO chip (default /dev/gpiochip0)\n"
          "  -r, --reset-line N      BCM reset GPIO (default 25)\n\n"
          "KEY_HEX is 12 hex digits; DATA_HEX is 32 hex digits. Writes to\n"
          "block 0 and sector trailers are intentionally refused.\n",
          program, program, program, program);
}

int main(int argc, char **argv) {
  static const struct option options[] = {
      {"device", required_argument, NULL, 'd'},
      {"speed", required_argument, NULL, 's'},
      {"gpiochip", required_argument, NULL, 'g'},
      {"reset-line", required_argument, NULL, 'r'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0},
  };
  const char *spi_path = "/dev/spidev0.0";
  const char *gpiochip = "/dev/gpiochip0";
  uint32_t speed = 1000000;
  unsigned int reset_line = 25;
  struct device dev = {.spi_fd = -1, .gpio_fd = -1};
  struct card card;
  int option, result = EXIT_FAILURE;

  while ((option = getopt_long(argc, argv, "d:s:g:r:h", options, NULL)) != -1) {
    char *end;
    unsigned long value;
    switch (option) {
    case 'd':
      spi_path = optarg;
      break;
    case 'g':
      gpiochip = optarg;
      break;
    case 's':
      value = strtoul(optarg, &end, 0);
      if (!*optarg || *end || value == 0 || value > 10000000)
        goto bad_arguments;
      speed = (uint32_t)value;
      break;
    case 'r':
      value = strtoul(optarg, &end, 0);
      if (!*optarg || *end || value > UINT32_MAX)
        goto bad_arguments;
      reset_line = (unsigned int)value;
      break;
    case 'h':
      usage(stdout, argv[0]);
      return EXIT_SUCCESS;
    default:
      goto bad_arguments;
    }
  }
  if (optind >= argc)
    goto bad_arguments;
  if (open_device(&dev, spi_path, gpiochip, reset_line, speed) != 0)
    goto out;

  if (!strcmp(argv[optind], "version") && optind + 1 == argc) {
    uint8_t version;
    if (read_reg(&dev, VersionReg, &version) != 0)
      goto failed;
    printf("version=0x%02x\n", version);
    if (version == 0 || version == 0xff)
      goto failed;
    result = EXIT_SUCCESS;
    goto out;
  }
  if (detect_card(&dev, &card) != 0) {
    fprintf(stderr, "no ISO14443A card detected: %s\n", strerror(errno));
    goto out;
  }
  print_card(&card);
  if (!strcmp(argv[optind], "scan") && optind + 1 == argc) {
    result = EXIT_SUCCESS;
    goto out;
  }
  if ((!strcmp(argv[optind], "read") && optind + 4 == argc) ||
      (!strcmp(argv[optind], "write") && optind + 5 == argc)) {
    uint8_t block, key[6], data[16];
    uint8_t key_command;
    if (parse_block(argv[optind + 1], &block) != 0 ||
        (strcmp(argv[optind + 2], "A") && strcmp(argv[optind + 2], "B")) ||
        parse_hex(argv[optind + 3], key, sizeof(key)) != 0)
      goto bad_arguments;
    key_command = !strcmp(argv[optind + 2], "A") ? PICC_MF_AUTH_KEY_A
                                                 : PICC_MF_AUTH_KEY_B;
    if (authenticate(&dev, &card, block, key_command, key) != 0) {
      fprintf(stderr, "authentication failed: %s\n", strerror(errno));
      goto out;
    }
    if (!strcmp(argv[optind], "read")) {
      if (read_block(&dev, block, data) != 0)
        goto failed;
      printf("block=%u data=", block);
      print_hex(data, sizeof(data));
      putchar('\n');
    } else {
      if (parse_hex(argv[optind + 4], data, sizeof(data)) != 0)
        goto bad_arguments;
      if (write_block(&dev, block, data) != 0)
        goto failed;
      printf("block=%u written\n", block);
    }
    clear_bits(&dev, Status2Reg, 0x08);
    result = EXIT_SUCCESS;
    goto out;
  }
bad_arguments:
  usage(stderr, argv[0]);
  goto out;
failed:
  fprintf(stderr, "MFRC522 operation failed: %s\n", strerror(errno));
out:
  close_device(&dev);
  return result;
}
