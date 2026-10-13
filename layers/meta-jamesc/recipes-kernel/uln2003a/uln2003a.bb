SUMMARY = "Device-tree GPIO half-step driver for ULN2003A unipolar steppers"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"
SRC_URI = "file://Makefile file://uln2003a.c"
S = "${WORKDIR}"
inherit module
KERNEL_MODULE_AUTOLOAD += "uln2003a"
COMPATIBLE_MACHINE = "^raspberrypi4-64$"
