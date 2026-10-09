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
accept firmware bundles up to 384 MiB.

The default password and MAC-derived OTA token are development credentials.
Before using an untrusted network, change both credentials, terminate TLS in
front of the service, and set `DEVICE_WEB_SECURE_COOKIES=true`.
