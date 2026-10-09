SUMMARY = "Base image for James C embedded platforms"
DESCRIPTION = "Project image derived from core-image-base and ready for platform-specific additions."
LICENSE = "MIT"

require recipes-core/images/core-image-base.bb

# Keep the kernel in each rootfs slot so it is updated atomically with userspace.
# U-Boot loads it from /boot in the selected ext4 partition.
IMAGE_INSTALL:append = " \
    curl \
    device-web \
    kernel-image \
    jamesc-ota-api \
    libubootenv-bin \
    mfrc522-tool \
    rauc \
    rauc-health \
    u-boot-env \
    util-linux-findmnt \
"

# U-Boot reads the per-slot copy; do not also place a stale shared kernel on
# the FAT partition.
RPI_EXTRA_IMAGE_BOOT_FILES:remove = "${KERNEL_IMAGETYPE}"

# meta-rauc recommends its generic mark-good service. The platform service
# below replaces it so product health checks can gate slot confirmation.
BAD_RECOMMENDATIONS += "rauc-mark-good"
