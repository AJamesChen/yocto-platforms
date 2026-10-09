SUMMARY = "Raspberry Pi MFRC522 SPI device-tree overlay"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://mfrc522-overlay.dts"

S = "${WORKDIR}"

inherit deploy

DEPENDS = "dtc-native"

do_compile() {
    ${STAGING_BINDIR_NATIVE}/dtc -@ -I dts -O dtb \
        -o ${WORKDIR}/mfrc522.dtbo ${S}/mfrc522-overlay.dts
}

do_deploy() {
    install -Dm 0644 ${WORKDIR}/mfrc522.dtbo ${DEPLOYDIR}/mfrc522.dtbo
}

addtask deploy after do_compile before do_build

COMPATIBLE_MACHINE = "^raspberrypi4-64$"
