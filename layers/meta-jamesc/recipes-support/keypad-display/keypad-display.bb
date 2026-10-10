SUMMARY = "4x4 matrix keypad with HD44780 display output"
DESCRIPTION = "Scans a 4x4 matrix keypad through libgpiod and displays key presses on an HD44780 LCD."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://keypad-display.c \
    file://keypad-display.service \
"

S = "${WORKDIR}"

inherit pkgconfig systemd

DEPENDS = "libgpiod"

SYSTEMD_SERVICE:${PN} = "keypad-display.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_compile() {
    ${CC} ${CFLAGS} ${CPPFLAGS} `pkg-config --cflags libgpiod` \
        -o keypad-display ${S}/keypad-display.c \
        ${LDFLAGS} `pkg-config --libs libgpiod`
}

do_install() {
    install -Dm 0755 ${B}/keypad-display ${D}${bindir}/keypad-display
    install -Dm 0644 ${WORKDIR}/keypad-display.service \
        ${D}${systemd_system_unitdir}/keypad-display.service
}
