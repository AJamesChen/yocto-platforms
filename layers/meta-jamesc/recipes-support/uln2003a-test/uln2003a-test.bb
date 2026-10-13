SUMMARY = "ULN2003A direction and speed test harness"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
SRC_URI = "file://uln2003a-test.py"
S = "${WORKDIR}"
RDEPENDS:${PN} = "python3-core"
do_install() {
    install -Dm 0755 ${S}/uln2003a-test.py ${D}${bindir}/uln2003a-test
}
