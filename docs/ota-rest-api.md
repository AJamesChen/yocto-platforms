# REST API for system information and OTA updates

The `jamesc-ota-api` service provides a small authenticated HTTP interface to
system information and RAUC installation. It listens on `127.0.0.1:8080` by
default. Keeping the service on loopback prevents a bearer token and firmware
bundle from crossing the network as plaintext.

## Connect from a development host

The project script performs the complete tunnel, upload, status polling, and
optional reboot workflow. First build the signed update bundle, then run:

```sh
./scripts/build-rpi4b.sh rpi4-update-bundle
./scripts/flash-ota-rpi4b.sh --reboot PI_ADDRESS
```

By default the script reads the development OTA token from the device over SSH.
Pass `--token TOKEN` (or set `OTA_API_TOKEN`) when that is not appropriate. Run
`./scripts/flash-ota-rpi4b.sh --help` for bundle, port, and timeout options.

The commands below show the equivalent workflow step by step.

In a host terminal, create an SSH tunnel to the API's loopback address on the
Pi. The explicit IPv4 bind also works on hosts where SSH cannot bind to IPv6
localhost. Leave this command running:

```sh
ssh -4 -N -L 127.0.0.1:18081:127.0.0.1:8080 root@PI_ADDRESS
```

The temporary bearer token is the lowercase Ethernet MAC address with its
colons removed. The configured MAC address is `DC:A6:32:7C:4E:1E`, so set the
fixed token in a second host terminal:

```sh
TOKEN=dca6327c4e1e
```

The image includes `curl`. Older images that do not include it can still be
updated from the host through this tunnel.

## Endpoints

The liveness endpoint does not require authentication:

```sh
curl --fail http://127.0.0.1:18081/api/v1/health
```

All other endpoints require the MAC-address bearer token.

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
automatically. Reboot only after `succeeded` by sending an authenticated POST:

```sh
curl --fail-with-body -X POST \
  -H "Authorization: Bearer ${TOKEN}" \
  http://127.0.0.1:18081/api/v1/reboot
```

The endpoint returns HTTP `202` with `{"state":"rebooting"}` and schedules
an orderly systemd reboot after a two-second delay so the response can reach
the client. It returns HTTP `409` while a firmware upload or RAUC installation
is active.

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

The last successful update time and uploaded bundle live under `/data/ota`, so
they survive rootfs slot changes. At every service start, the initializer writes
`OTA_API_TOKEN` from `/etc/default/jamesc-ota-api` to `/data/ota/api-token`;
this also migrates devices that already have a random token from an earlier
image.

The token is always stored as 12 lowercase hexadecimal characters without
separators. Using a MAC address as a bearer token is intended only for temporary
development use: MAC addresses are easy to discover and provide identification,
not secret authentication. Replace this scheme with per-device credentials
before exposing the update service beyond a trusted development environment.
