SUMMARY = "Web console for system, camera, and RAUC firmware management"
DESCRIPTION = "Authenticated Go web service that displays device status, controls the V4L2 camera, and proxies firmware updates to jamesc-ota-api."
HOMEPAGE = "https://github.com/AJamesChen/device-web"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://src/${GO_IMPORT}/LICENSE;md5=28ebbb63d463670ad7108315ab40ca77"

PV = "1.0+git"
SRC_URI = "git://github.com/AJamesChen/device-web.git;branch=main;protocol=https"
SRCREV = "48d63747ea04485115a27fb10eba5d7df9c2ea73"

GO_IMPORT = "github.com/AJamesChen/device-web"
GO_INSTALL = "${GO_IMPORT}/cmd/device-web"

RDEPENDS:${PN} = " \
    jamesc-ota-api \
    v4l-utils \
    gstreamer1.0 \
    gstreamer1.0-plugins-base-pango \
    gstreamer1.0-plugins-base-videoconvertscale \
    gstreamer1.0-plugins-good-jpeg \
    gstreamer1.0-plugins-good-video4linux2 \
    cantarell-fonts \
"

inherit go-mod systemd

SYSTEMD_SERVICE:${PN} = "device-web.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install:append() {
    install -d ${D}${sysconfdir}/default
    install -m 0644 ${S}/src/${GO_IMPORT}/deploy/device-web.default \
        ${D}${sysconfdir}/default/device-web

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/src/${GO_IMPORT}/deploy/device-web.service \
        ${D}${systemd_system_unitdir}/
}
