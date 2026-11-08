SUMMARY = "DHCP configuration for wired Jetson interfaces"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

inherit allarch

SRC_URI = "file://20-wired.network"
S = "${WORKDIR}"

RDEPENDS:${PN} = "systemd-networkd"
CONFFILES:${PN} = "${sysconfdir}/systemd/network/20-wired.network"

do_install() {
    install -d ${D}${sysconfdir}/systemd/network
    install -m 0644 ${WORKDIR}/20-wired.network ${D}${sysconfdir}/systemd/network/20-wired.network
}

FILES:${PN} = "${sysconfdir}/systemd/network/20-wired.network"
