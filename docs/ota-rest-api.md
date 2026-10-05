# REST API for system information and OTA updates

The `jamesc-ota-api` service provides a small authenticated HTTP interface to
system information and RAUC installation. It listens on `127.0.0.1:8080` by
default. Keeping the service on loopback prevents a bearer token and firmware
bundle from crossing the network as plaintext.

## Connect from a development host

In a host terminal, create an SSH tunnel to the API's loopback address on the
Pi. The explicit IPv4 bind also works on hosts where SSH cannot bind to IPv6
localhost. Leave this command running:

```sh
ssh -4 -N -L 127.0.0.1:18081:127.0.0.1:8080 root@PI_ADDRESS
```

In a second host terminal, read the token into a shell variable. Avoid printing
the token because it grants access to the update API:

```sh
TOKEN=$(ssh root@PI_ADDRESS cat /data/ota/api-token)
```

The image includes `curl`. Older images that do not include it can still be
updated from the host through this tunnel.

## Endpoints

The liveness endpoint does not require authentication:

```sh
curl --fail http://127.0.0.1:18081/api/v1/health
```

All other endpoints require the generated bearer token.

Get system information, including firmware metadata, the active RAUC slot,
CPU details and load, and memory use:

```sh
curl --fail \
  -H "Authorization: Bearer ${TOKEN}" \
  http://127.0.0.1:18081/api/v1/system
```

Example response (formatted for readability):

```json
{
  "hostname": "raspberrypi4-64",
  "os": "Poky (Yocto Project Reference Distro) 5.0.20 (scarthgap)",
  "firmware": {
    "version": "5.0.20",
    "build_id": "20261005121550",
    "built_at": "2026-10-05T12:15:50Z",
    "updated_at": "2026-10-05T13:04:22Z"
  },
  "cpu": {
    "model": "ARMv8 Processor rev 3 (v8l)",
    "architecture": "aarch64",
    "logical_cores": 4,
    "load_1m": 0.08,
    "load_5m": 0.04,
    "load_15m": 0.01
  },
  "memory": {
    "total_bytes": 8589934592,
    "available_bytes": 7990149120,
    "used_bytes": 599785472,
    "usage_percent": 7.0
  },
  "kernel": "6.6.31-v8",
  "rauc_slot": "A",
  "root": "/dev/mmcblk0p2",
  "uptime_seconds": 931
}
```

`built_at` is derived from the firmware image's Yocto build ID. `updated_at`
is written to persistent `/data` after a successful REST-triggered RAUC
installation; before the first OTA update it falls back to `built_at`. Memory
use is calculated as `MemTotal - MemAvailable` from `/proc/meminfo`.

Get the current upload or installation state:

```sh
curl --fail \
  -H "Authorization: Bearer ${TOKEN}" \
  http://127.0.0.1:18081/api/v1/update
```

Upload and install a signed bundle as the raw request body:

```sh
curl --fail-with-body \
  -H "Authorization: Bearer ${TOKEN}" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @build/rpi4b/build/tmp/deploy/images/raspberrypi4-64/rpi4-update-bundle-raspberrypi4-64.raucb \
  http://127.0.0.1:18081/api/v1/update
```

A successful upload returns HTTP `202` with an `installing` state. The HTTP
request stays open while the bundle uploads. To monitor progress from a third
host terminal, poll the status endpoint; `bytes_received` should increase:

```sh
curl --fail \
  -H "Authorization: Bearer ${TOKEN}" \
  http://127.0.0.1:18081/api/v1/update
```

After the upload completes, continue polling until the state is `succeeded`.
RAUC verifies the bundle signature and compatible string before writing the
inactive slot. A failed verification or installation is reported as `failed`
with the RAUC process exit code. The update does not reboot the Pi
automatically. Reboot only after `succeeded`:

```sh
ssh root@PI_ADDRESS reboot
```

After the Pi comes back, check `/proc/cmdline` for the other `rauc.slot` and
query `/api/v1/system` again to confirm the running firmware version and slot.

Only one upload or installation is accepted at a time. The default upload
limit is 384 MiB and can be changed in `/etc/default/jamesc-ota-api`.

## Network deployment

Do not expose this service directly over an untrusted network. HTTP bearer
tokens are not confidential without transport encryption. For a deployed
product, keep the service on loopback and place an HTTPS reverse proxy with
device authentication, authorization, request limits, and audit logging in
front of it. If a trusted development LAN requires direct access, change
`OTA_API_BIND` to `0.0.0.0` and restart the service:

```sh
systemctl restart jamesc-ota-api.service
```

The token, last successful update time, and uploaded bundle live under
`/data/ota`, so they survive rootfs slot changes. The first service start
creates a 64-character random token. The initializer replaces malformed token
files, including those created by earlier images that used unsupported BusyBox
`od` options.
