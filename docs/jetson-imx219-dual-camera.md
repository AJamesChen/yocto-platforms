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
are not provided by the kernel driver itself.

## Live preview and calibrated depth

The Orin Nano image includes `stereo-camera-viewer`, a small headless viewer
that captures Argus sensors 0 and 1 and serves the feeds on the wired network.
After booting an image that contains the viewer, open:

```text
http://<jetson-ip>:8080/
```

For example, with the board at `192.168.68.53`, use
`http://192.168.68.53:8080/`. The page shows the left and right feeds, plus a
relative disparity preview until a stereo calibration is loaded. After
calibration, it shows a color-coded depth map in meters. The app requires both
cameras and the NVIDIA Argus daemon; check it with:

```sh
systemctl status nvargus-daemon stereo-camera-viewer --no-pager
journalctl -u stereo-camera-viewer -b --no-pager
```

To view the page through SSH without exposing port 8080 on the LAN, run this
on the development machine and leave the SSH session open:

```sh
ssh -L 127.0.0.1:8080:127.0.0.1:8080 root@JETSON_IP
```

Then open `http://127.0.0.1:8080/` in the development machine's browser. This
works only after booting the image that contains the `stereo-camera-viewer`
service; an SSH tunnel cannot provide the viewer when the service is absent or
stopped.

## Chessboard calibration

The viewer defaults to a board with 9 inner corners across, 6 down, and
25 mm-wide squares. Use a printed board with those dimensions, or change the
settings in the page to match the board. Measure the printed squares; the
entered size sets the scale of the depth values.

1. Keep the board still and visible in both feeds. Capture at least 12 pairs
   with the board at different distances, tilts, and positions across the
   image. Move the board between captures and avoid glare or motion blur.
2. Select **Calibrate and save**. The viewer estimates each camera's intrinsics,
   the stereo relationship, and rectification maps. It shows the reprojection
   RMS and estimated camera baseline when calibration completes.
3. The calibrated depth map is colored from 0.2 m to 5 m by default. Click a
   pixel in the depth panel to read the median depth of nearby valid pixels.
   Black regions mean the stereo matcher could not find a valid match or the
   depth is outside that range.

The calibration is saved at
`/data/stereo-camera-viewer/calibration.npz` and loaded at service startup. It
is for the viewer's 640x480 capture resolution; recalibrate if that resolution
changes. Use **Clear samples and calibration** before starting over or after
changing the board dimensions.

The calculated scale depends on the measured square size and calibration
quality. Depth can be noisy on low-texture, reflective, or moving objects. The
two CSI streams are not guaranteed to be hardware-synchronized; use a
stationary scene for best results. This is a calibrated stereo depth preview,
not a certified measurement instrument. The web page and calibration controls
have no authentication, so use them only on a trusted development network or
access them through the documented SSH tunnel.

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

For CSI cameras, NVIDIA's Argus pipeline is the capture path used by the
viewer. A missing `/dev/video*` glob alone is not enough to diagnose capture;
check the Argus and viewer service status and logs, and try the browser preview.

Both physical cameras must be connected for the dual overlay. If no camera is
detected, check cable compatibility and orientation first, then inspect the
kernel log for I2C or CSI errors.
