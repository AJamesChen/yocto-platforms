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
and uptime information. A signed `.raucb` bundle can be uploaded from the
firmware update panel. The web service forwards system and update requests to
the loopback-only `jamesc-ota-api`; the OTA bearer token is not sent to the
browser.

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
