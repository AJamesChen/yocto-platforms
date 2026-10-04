SUMMARY = "Confirm a successfully booted RAUC slot"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302c"

SRC_URI = " \
    file://jamesc-rauc-mark-good.service \
    file://rauc-mark-good.sh \
"

inherit systemd

RDEPENDS:${PN} = "rauc util-linux-mount"
SYSTEMD_SERVICE:${PN} = "jamesc-rauc-mark-good.service"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/rauc-mark-good.sh ${D}${bindir}/rauc-mark-good

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/jamesc-rauc-mark-good.service \
        ${D}${systemd_system_unitdir}/
}
