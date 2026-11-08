# Dual IMX219 camera on Jetson Orin Nano

The `jetson-orin-nano-devkit` image enables NVIDIA's dual-IMX219 device-tree
overlay:

```text
tegra234-p3767-camera-p3768-imx219-dual.dtbo
```

The overlay is referenced by the generated `/boot/extlinux/extlinux.conf` and
applied by L4TLauncher at boot. The image explicitly installs the BSP's
`nv-kernel-module-nv-imx219` package, which provides `nv_imx219.ko`; no custom
sensor-driver recipe is needed for supported IMX219 modules.

## Hardware

This setup targets the NVIDIA Orin Nano developer-kit carrier board. Connect
the two sensors to its CAM0 and CAM1 CSI connectors using compatible flex cables
for the carrier's 22-position, 0.5 mm connectors. Power off the Jetson before
connecting or removing the cables, and follow the camera module's cable
orientation instructions. The Waveshare IMX219-83 binocular camera is one
example of a dual-sensor module.

The overlay configures both camera interfaces as IMX219 sensors. Stereo vision
applications and sensor synchronization are separate from kernel detection and
are not configured by this image change.

## Verify after boot

Confirm that the overlay is referenced:

```sh
grep -n 'OVERLAYS' /boot/extlinux/extlinux.conf
```

Check driver and sensor detection:

```sh
modinfo nv_imx219
dmesg | grep -iE 'imx219|tegra-capture|nvcsi'
ls -l /dev/video*
```

Both physical cameras must be connected for the dual overlay. If no camera is
detected, check cable compatibility and orientation first, then inspect the
kernel log for I2C or CSI errors.
