SUMMARY = "HC-SR501 motion detector with HD44780 display output"
DESCRIPTION = "Monitors a PIR sensor through libgpiod and displays a motion alert on an HD44780 LCD."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://motion-display.c \
    file://motion-display.service \
"

S = "${WORKDIR}"

inherit pkgconfig systemd

DEPENDS = "libgpiod"

SYSTEMD_SERVICE:${PN} = "motion-display.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_compile() {
    ${CC} ${CFLAGS} ${CPPFLAGS} `pkg-config --cflags libgpiod` \
        -o motion-display ${S}/motion-display.c \
        ${LDFLAGS} `pkg-config --libs libgpiod`
}

do_install() {
    install -Dm 0755 ${B}/motion-display ${D}${bindir}/motion-display
    install -Dm 0644 ${WORKDIR}/motion-display.service \
        ${D}${systemd_system_unitdir}/motion-display.service
}
