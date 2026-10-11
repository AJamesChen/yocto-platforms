do_install:append:raspberrypi4-64() {
    # U-Boot stores its persistent environment on the shared FAT partition.
    # RAUC's U-Boot backend therefore needs that partition available at /boot
    # when it invokes fw_printenv and fw_setenv.
    printf '%s\n' 'LABEL=boot /boot vfat defaults 0 2' >> ${D}${sysconfdir}/fstab

    # Persist RAUC state, OTA uploads, and the device token independently of
    # either replaceable rootfs slot. The mark-good health check requires this
    # mount before it confirms a newly booted slot.
    printf '%s\n' 'LABEL=data /data ext4 defaults 0 2' >> ${D}${sysconfdir}/fstab
}
