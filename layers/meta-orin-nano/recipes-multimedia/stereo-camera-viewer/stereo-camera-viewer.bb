SUMMARY = "Headless dual CSI camera calibration and metric depth viewer"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://stereo-camera-viewer.py \
           file://stereo-camera-viewer.service \
"
S = "${UNPACKDIR}"

inherit systemd

RDEPENDS:${PN} = " \
    python3-core \
    python3-netclient \
    python3-netserver \
    python3-threading \
    python3-numpy \
    python3-opencv \
    tegra-argus-daemon \
    gstreamer1.0-plugins-nvarguscamerasrc \
    gstreamer1.0-plugins-nvvidconv \
    gstreamer1.0-plugins-base \
"

do_install() {
    install -D -m 0755 ${S}/stereo-camera-viewer.py ${D}${bindir}/stereo-camera-viewer
    install -D -m 0644 ${S}/stereo-camera-viewer.service ${D}${systemd_system_unitdir}/stereo-camera-viewer.service
}

SYSTEMD_SERVICE:${PN} = "stereo-camera-viewer.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"
