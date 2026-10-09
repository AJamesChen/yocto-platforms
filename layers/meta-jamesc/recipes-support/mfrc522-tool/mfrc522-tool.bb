SUMMARY = "MFRC522 RFID reader and MIFARE Classic block utility"
DESCRIPTION = "Userspace SPI/GPIO driver and command-line tool for the MFRC522 RFID reader."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://mfrc522-tool.c"

S = "${WORKDIR}"

do_compile() {
    ${CC} ${CFLAGS} ${CPPFLAGS} ${LDFLAGS} \
        -o mfrc522-tool ${S}/mfrc522-tool.c
}

do_install() {
    install -Dm 0755 ${B}/mfrc522-tool ${D}${bindir}/mfrc522-tool
}
