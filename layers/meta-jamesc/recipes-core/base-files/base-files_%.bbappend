do_install:append:raspberrypi4-64() {
    # U-Boot stores its persistent environment on the shared FAT partition.
    # RAUC's U-Boot backend therefore needs that partition available at /boot
    # when it invokes fw_printenv and fw_setenv.
    printf '%s\n' 'LABEL=boot /boot vfat defaults 0 2' >> ${D}${sysconfdir}/fstab
}
