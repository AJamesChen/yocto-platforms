SUMMARY = "REST API for system information, RAUC updates, and reboot"
DESCRIPTION = "Authenticated HTTP service that reports system state, streams signed RAUC bundles into the inactive slot, and requests an orderly reboot."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://ota-api.c \
    file://jamesc-ota-api-init \
    file://jamesc-ota-api.service \
    file://jamesc-ota-api.default \
"

DEPENDS = "libmicrohttpd"
RDEPENDS:${PN} = "rauc systemd"

inherit pkgconfig systemd

SYSTEMD_SERVICE:${PN} = "jamesc-ota-api.service"

do_compile() {
    ${CC} ${CFLAGS} \
        -fdebug-prefix-map=${WORKDIR}=/usr/src/debug/${PN}/${PV} \
        -fmacro-prefix-map=${WORKDIR}=/usr/src/debug/${PN}/${PV} \
        ${WORKDIR}/ota-api.c -o jamesc-ota-api \
        ${LDFLAGS} `pkg-config --cflags --libs libmicrohttpd` -pthread
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 jamesc-ota-api ${D}${bindir}/jamesc-ota-api

    install -d ${D}${libexecdir}
    install -m 0755 ${WORKDIR}/jamesc-ota-api-init \
        ${D}${libexecdir}/jamesc-ota-api-init

    install -d ${D}${sysconfdir}/default
    install -m 0644 ${WORKDIR}/jamesc-ota-api.default \
        ${D}${sysconfdir}/default/jamesc-ota-api

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/jamesc-ota-api.service \
        ${D}${systemd_system_unitdir}/
}
