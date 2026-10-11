# Legacy V4L2 camera on Raspberry Pi 4B

The Raspberry Pi 4B image enables the firmware-based legacy camera stack and
exposes a supported CSI camera through the `bcm2835-v4l2` driver. This is the
same MMAL-based path used by older Raspberry Pi images: applications capture
from `/dev/video0` with the standard V4L2 API.

The legacy stack supports Camera Module 1, Camera Module 2, and the High
Quality Camera. It does not support newer Camera Module 3 sensors. Power off
the Pi before connecting or reversing the CSI ribbon cable.

## Boot and driver configuration

The board configuration sets:

```text
start_x=1
gpu_mem=128
camera_auto_detect=0
```

`VIDEO_CAMERA = "1"` generates `start_x=1`, selecting `start4x.elf` and
`fixup4x.dat` on Raspberry Pi 4. `camera_auto_detect=0` prevents the firmware
from applying a modern sensor/Unicam overlay at the same time. The two camera
stacks must not own the sensor concurrently.

The base Raspberry Pi device tree already enables the VCHIQ firmware transport
used by the legacy camera. Unlike the modern libcamera path, the legacy stack
does not use an `imx219`, `ov5647`, or `imx477` sensor overlay. Camera control
is handled by VideoCore firmware, and `bcm2835-v4l2` provides the Linux V4L2
interface.

The image explicitly installs and autoloads `bcm2835-v4l2`, and includes
`v4l2-ctl` from `v4l-utils` for inspection and capture tests.

## Build and deploy

Because `config.txt` and the camera firmware live on the shared FAT boot
partition, a root-filesystem-only RAUC update cannot enable `start_x` on an
existing card. Build and flash a new WIC image for the first camera-enabled
deployment:

```sh
./scripts/build-rpi4b.sh core-image-jamesc
bzcat build/rpi4b/build/tmp/deploy/images/raspberrypi4-64/core-image-jamesc-raspberrypi4-64.rootfs.wic.bz2 \
  | sudo dd of=/dev/SD_DEVICE bs=4M conv=fsync status=progress
```

Replace `/dev/SD_DEVICE` with the whole SD-card device, not a partition. Later
root-filesystem changes can continue to use the normal RAUC update workflow.

## Verify on the Pi

After booting the newly flashed image, run the project test script:

```sh
scp scripts/test-v4l2-camera-rpi4b.sh root@PI_ADDRESS:/tmp/
ssh root@PI_ADDRESS /tmp/test-v4l2-camera-rpi4b.sh
scp root@PI_ADDRESS:/tmp/v4l2-camera-test.jpg .
```

The script verifies the boot configuration, loads the driver, prints the V4L2
device information, and captures one 640x480 JPEG frame. A successful result
ends with the output path and a non-zero byte count.

## Validated result

This path was exercised on the project Raspberry Pi 4B with Linux 6.6.63. The
driver registered `/dev/video0` as `bcm2835 mmal`, reported `Camera 0: ok`, and
advertised JPEG/MJPEG capture up to 2592x1944. The test captured and decoded a
640x480 JPEG frame successfully.

For manual diagnostics:

```sh
grep -E '^(start_x|gpu_mem|camera_auto_detect)=' /boot/config.txt
lsmod | grep bcm2835_v4l2
v4l2-ctl --list-devices
v4l2-ctl --device=/dev/video0 --all
dmesg | grep -Ei 'bcm2835|camera|mmal|vchiq'
```

If `/dev/video0` is absent, confirm the CSI ribbon orientation and reseat both
ends with power removed. If the driver reports that no camera is detected,
verify that the module is one supported by the legacy stack and that
`start4x.elf` and `fixup4x.dat` are present on the boot partition.
