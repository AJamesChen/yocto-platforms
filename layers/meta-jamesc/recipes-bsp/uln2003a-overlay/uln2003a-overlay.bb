SUMMARY = "Raspberry Pi ULN2003A stepper device-tree overlay"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
SRC_URI = "file://uln2003a-overlay.dts"
S = "${WORKDIR}"
inherit deploy
DEPENDS = "dtc-native"
do_compile() {
    ${STAGING_BINDIR_NATIVE}/dtc -@ -I dts -O dtb \
        -o ${WORKDIR}/uln2003a.dtbo ${S}/uln2003a-overlay.dts
}
do_deploy() {
    install -Dm 0644 ${WORKDIR}/uln2003a.dtbo ${DEPLOYDIR}/uln2003a.dtbo
}
addtask deploy after do_compile before do_build
COMPATIBLE_MACHINE = "^raspberrypi4-64$"
