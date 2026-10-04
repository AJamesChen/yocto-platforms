SUMMARY = "Signed rootfs-only RAUC update bundle for Raspberry Pi 4"
LICENSE = "MIT"

inherit bundle

RAUC_BUNDLE_COMPATIBLE = "jamesc-raspberrypi4-64"
RAUC_BUNDLE_VERSION = "${DISTRO_VERSION}"
RAUC_BUNDLE_DESCRIPTION = "James C Raspberry Pi 4 root filesystem"
RAUC_BUNDLE_FORMAT = "verity"

RAUC_BUNDLE_SLOTS = "rootfs"
RAUC_SLOT_rootfs = "core-image-jamesc"
RAUC_SLOT_rootfs[fstype] = "ext4"

# Demonstration credentials make the example directly buildable. Replace both
# files and the target keyring before distributing a production device.
RAUC_KEY_FILE = "${THISDIR}/files/development.key.pem"
RAUC_CERT_FILE = "${THISDIR}/files/development.cert.pem"
