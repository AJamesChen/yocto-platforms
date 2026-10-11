# Device web console

`core-image-jamesc` includes the Go-based `device-web` service. BitBake fetches
the pinned source revision, cross-compiles the binary for the target, and
enables `device-web.service` automatically.

After boot, open the following address from another machine on the same trusted
network:

```text
http://PI_ADDRESS:8081
```

The development login is:

```text
Username: admin
Password: jamesc
```

The dashboard displays firmware, RAUC slot, CPU, memory, kernel, root device,
and uptime information. A signed `.raucb` bundle can be selected or dragged
onto the firmware update panel. The browser validates the extension, size, and
single-file selection before upload. The web service forwards system and
update requests to the loopback-only `jamesc-ota-api`; the OTA bearer token is
not sent to the browser.

The **Camera** tab displays the V420 feed from `/dev/video0`. Use **Play video**
to start the MJPEG stream, **Stop playback** to release the camera, and **Snap
picture** to capture and download a 640x480 JPEG. The service runs with the
supplementary `video` group and supports both `v4l2-ctl` and GStreamer capture.

The camera controls are also available through the authenticated REST API:

| Method | Endpoint | Response |
| --- | --- | --- |
| `GET` | `/api/camera` | State, device, resolution, and frame rate JSON |
| `GET` | `/api/camera/stream` | Live multipart MJPEG stream |
| `POST` | `/api/camera/stop` | Stop capture and release the device |
| `POST` | `/api/camera/snapshot` | One `image/jpeg` frame |

All four endpoints require the login session cookie. The `POST` endpoints also
require the `csrf_token` returned by `GET /api/session` in an
`X-CSRF-Token` header. Only one camera operation runs at a time; stop playback
before requesting a snapshot from an API client.

Before starting an update, confirm that RAUC can access the shared U-Boot
environment:

```sh
findmnt /boot
fw_printenv BOOT_ORDER BOOT_A_LEFT BOOT_B_LEFT
rauc status
```

Keep the device powered while the page reports `uploading` or `installing`.
The page continues polling the OTA service and reports either successful
installation or failure with the RAUC exit code. If the browser loses the
upload response, it checks the authoritative device-side status before showing
an error, so a completed installation is not reported as a connection failure.

Reboot only after it reports `succeeded`. Use the **Reboot** button in the
header and confirm the prompt. The web service rejects reboot requests while
an upload or installation is active, waits for the device to go offline, and
returns to the login page when the device is reachable again.

After reboot, verify the new slot, automatic FAT mount, and health confirmation:

```sh
tr ' ' '\n' </proc/cmdline | grep '^rauc.slot='
findmnt /boot
rauc status
systemctl is-active jamesc-rauc-mark-good.service device-web.service
```

## Build and verify

Build the image or just the package:

```sh
./scripts/build-rpi4b.sh core-image-jamesc
./scripts/build-rpi4b.sh device-web
```

On the Pi, confirm that the service was enabled and started:

```sh
systemctl status device-web.service
curl --fail http://127.0.0.1:8081/health
journalctl -u device-web.service
```

Runtime settings live in `/etc/default/device-web`. The defaults connect to the
OTA API at `127.0.0.1:8080`, listen for web connections on port `8081`, and
accept firmware bundles up to 384 MiB. Camera settings are controlled by
`DEVICE_WEB_CAMERA_DEVICE`, `DEVICE_WEB_CAMERA_WIDTH`,
`DEVICE_WEB_CAMERA_HEIGHT`, and `DEVICE_WEB_CAMERA_FPS`. Set
`DEVICE_WEB_CAMERA_BACKEND` to `v4l2` (the default) or `gstreamer`. The REST
camera status response includes the selected backend.

The GStreamer backend uses the camera's native MJPEG output without a decode
and re-encode step:

```sh
gst-launch-1.0 -q v4l2src device=/dev/video0 io-mode=mmap \
  ! 'image/jpeg,width=640,height=480,framerate=15/1' \
  ! fdsink fd=1 sync=false
```

The image includes the GStreamer command-line tools and `v4l2src` plugin.
After changing the backend in `/etc/default/device-web`, restart the service:

```sh
systemctl restart device-web.service
```

The default password and MAC-derived OTA token are development credentials.
Before using an untrusted network, change both credentials, terminate TLS in
front of the service, and set `DEVICE_WEB_SECURE_COOKIES=true`.
