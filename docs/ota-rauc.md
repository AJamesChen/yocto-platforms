# Raspberry Pi 4 A/B OTA with RAUC

This platform uses RAUC with U-Boot to update the root filesystem safely. An
update is written to the inactive slot, leaving the running slot untouched. On
the next boot U-Boot tries the new slot up to three times. If it never reaches
the health confirmation service, U-Boot falls back to the previous slot.
The image uses systemd so the RAUC daemon and health confirmation unit run
during normal startup.

## Storage layout

| Partition | Contents | Updated by RAUC |
| --- | --- | --- |
| `boot` | Raspberry Pi firmware, DTBs, U-Boot, boot script and environment | No |
| `rootfs-a` | Slot A rootfs and kernel | Yes, when inactive |
| `rootfs-b` | Slot B rootfs and kernel | Yes, when inactive |
| `data` | RAUC status and persistent application data | No |

The kernel is deliberately packaged in `/boot` inside each ext4 rootfs.
Although Linux mounts the FAT partition over `/boot` after startup, U-Boot
reads `boot/Image` directly from the selected rootfs partition. Thus a
rootfs-only bundle updates the userspace and matching kernel atomically. The
board DTBs remain shared on the FAT boot partition and are outside OTA scope.
The FAT mount also exposes `/boot/uboot.env` to `fw_printenv` and `fw_setenv`,
which RAUC's U-Boot backend uses to select and confirm A/B slots.

The initial SD-card image contains the same rootfs in slots A and B. It is a
bootstrap image; later deployments use only the much smaller `.raucb` file.

## Build and flash the bootstrap image

Prepare the source trees and build the project image:

```sh
./scripts/setup-rpi4b.sh
./scripts/build-rpi4b.sh
```

The deploy directory is:

```text
build/rpi4b/build/tmp/deploy/images/raspberrypi4-64/
```

Write the compressed WIC image with `bmaptool` (replace `/dev/sdX` with the
whole SD-card device, never a partition):

```sh
sudo bmaptool copy \
  build/rpi4b/build/tmp/deploy/images/raspberrypi4-64/core-image-jamesc-raspberrypi4-64.rootfs.wic.bz2 \
  /dev/sdX
sync
```

Alternatively, decompress through `bzcat` and `dd`:

```sh
bzcat build/rpi4b/build/tmp/deploy/images/raspberrypi4-64/core-image-jamesc-raspberrypi4-64.rootfs.wic.bz2 \
  | sudo dd of=/dev/sdX bs=4M status=progress conv=fsync
sudo partprobe /dev/sdX
lsblk -o NAME,SIZE,FSTYPE,LABEL,PARTLABEL /dev/sdX
```

## Build and install a rootfs update

After changing the image or application recipes, build a signed bundle:

```sh
./scripts/build-rpi4b.sh rpi4-update-bundle
```

Copy `rpi4-update-bundle-raspberrypi4-64.raucb` from the deploy directory to
the running Pi. Inspect and install it:

```sh
rauc info /tmp/rpi4-update-bundle-raspberrypi4-64.raucb
rauc status
rauc install /tmp/rpi4-update-bundle-raspberrypi4-64.raucb
reboot
```

The image also provides an authenticated REST interface for uploading and
installing the bundle. The helper script creates the required SSH tunnel,
uploads the default deploy artifact, waits for RAUC, and optionally reboots:

```sh
./scripts/flash-ota-rpi4b.sh --reboot PI_ADDRESS
```

See [REST API for system information and OTA updates](ota-rest-api.md) for the
manual commands and script options.

After reboot, verify the selected rootfs and RAUC state:

```sh
cat /proc/cmdline
findmnt /
rauc status
systemctl status jamesc-rauc-mark-good.service
fw_printenv BOOT_ORDER BOOT_A_LEFT BOOT_B_LEFT
```

`findmnt /boot` must report the FAT partition before installing an update. If
it is not mounted, RAUC cannot update `/boot/uboot.env` and will reject the
installation when marking the target slot non-bootable.

### Upgrade from an image without the `/boot` mount fix

Legacy development images may not mount the FAT partition automatically. On
such an image, mount it and restore the running slot before installing a
corrected bundle:

```sh
mount -t vfat /dev/disk/by-partlabel/boot /boot
fw_printenv BOOT_ORDER BOOT_A_LEFT BOOT_B_LEFT
rauc status mark-good
rauc status
```

Keep `/boot` mounted while running `rauc install`. Do not reboot until the
installation succeeds and `fw_printenv BOOT_ORDER` lists the updated inactive
slot first. The corrected image mounts `/boot` from `/etc/fstab` on subsequent
boots.

During development, a rootfs slot may generate a different SSH host key on its
first boot. If SSH reports that the host identity changed immediately after an
expected slot switch, verify the new fingerprint from a trusted local or
serial console before replacing the host's `known_hosts` entry.

The health policy initially marks a slot good after `boot-complete.target` when
`/data` is mounted. Before production, extend
`rauc-mark-good.sh` to check the streaming service, REST API, MQTT connection,
or another product-specific readiness signal.

## Exercise rollback

Perform this test only on a development device with serial access:

1. Install a test bundle and allow it to select the inactive slot.
2. Prevent the new slot from reaching the mark-good service (for example, use a
   deliberately failing development service dependency).
3. Reboot repeatedly and watch the U-Boot serial output. The selected slot's
   `BOOT_*_LEFT` counter decreases on each attempt.
4. After three failed attempts, U-Boot selects the other slot.
5. Run `rauc status` and `fw_printenv` to confirm the rollback.

Do not simulate failure by corrupting the partition table or the shared FAT
boot partition; neither belongs to an individual A/B slot.

## Development keys and production provisioning

The repository contains a public development certificate and its private key
so a fresh checkout can build and test bundles immediately. This key is public
and provides no production authenticity.

For production:

1. Generate the signing key in a protected signing environment. Do not commit
   it or copy it into normal developer workspaces.
2. Replace the development certificate installed by `rauc-conf.bbappend` with
   the production trust chain.
3. Pass the production `RAUC_KEY_FILE` and `RAUC_CERT_FILE` from a private build
   configuration or signing job.
4. Provision recovery and key-rotation procedures before devices leave the
   factory.

The signed bundle's `compatible` value and the target configuration are both
`jamesc-raspberrypi4-64`; RAUC rejects bundles for a different platform.

## Scope and limitations

This first implementation updates the complete rootfs only. It intentionally
does not update Raspberry Pi firmware, DTBs, U-Boot, the boot script, partition
table, or persistent data. Updating those shared components safely requires a
separate recovery design and should not be added to the rootfs bundle casually.
