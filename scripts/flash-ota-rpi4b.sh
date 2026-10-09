#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "${script_dir}/.." && pwd)
default_bundle="${project_dir}/build/rpi4b/build/tmp/deploy/images/raspberrypi4-64/rpi4-update-bundle-raspberrypi4-64.raucb"

bundle=${OTA_BUNDLE:-${default_bundle}}
token=${OTA_API_TOKEN:-}
local_port=${OTA_LOCAL_PORT:-18081}
poll_timeout=${OTA_POLL_TIMEOUT:-900}
reboot=false
tunnel_pid=

usage() {
    cat <<EOF
usage: $0 [options] PI_ADDRESS

Upload and install a Raspberry Pi 4 RAUC bundle through the loopback-only OTA
API. PI_ADDRESS may be a hostname, an address, or an SSH destination such as
root@192.0.2.10.

options:
  --bundle PATH   RAUC bundle to install (default: project deploy artifact)
  --token TOKEN   OTA bearer token (default: read from the device over SSH)
  --port PORT     Local SSH tunnel port (default: 18081)
  --reboot        Reboot the device after a successful installation
  -h, --help      Show this help

Environment equivalents: OTA_BUNDLE, OTA_API_TOKEN, OTA_LOCAL_PORT, and
OTA_POLL_TIMEOUT (seconds).
EOF
}

fail() {
    echo "error: $*" >&2
    exit 1
}

cleanup() {
    if [ -n "${tunnel_pid}" ]; then
        kill "${tunnel_pid}" 2>/dev/null || true
        wait "${tunnel_pid}" 2>/dev/null || true
    fi
}

while [ "$#" -gt 0 ]; do
    case $1 in
        --bundle)
            [ "$#" -ge 2 ] || fail "--bundle requires a path"
            bundle=$2
            shift 2
            ;;
        --token)
            [ "$#" -ge 2 ] || fail "--token requires a value"
            token=$2
            shift 2
            ;;
        --port)
            [ "$#" -ge 2 ] || fail "--port requires a value"
            local_port=$2
            shift 2
            ;;
        --reboot)
            reboot=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        -*)
            fail "unknown option: $1"
            ;;
        *)
            break
            ;;
    esac
done

[ "$#" -eq 1 ] || {
    usage >&2
    exit 2
}

ssh_target=$1
case ${ssh_target} in
    *@*) ;;
    *) ssh_target="root@${ssh_target}" ;;
esac

[ -f "${bundle}" ] || fail "bundle not found: ${bundle} (build it with ./scripts/build-rpi4b.sh rpi4-update-bundle)"
[ -r "${bundle}" ] || fail "bundle is not readable: ${bundle}"

case ${local_port} in
    ''|*[!0-9]*) fail "local port must be an integer" ;;
esac
[ "${local_port}" -ge 1 ] && [ "${local_port}" -le 65535 ] || fail "local port must be between 1 and 65535"

case ${poll_timeout} in
    ''|*[!0-9]*) fail "OTA_POLL_TIMEOUT must be an integer" ;;
esac
[ "${poll_timeout}" -gt 0 ] || fail "OTA_POLL_TIMEOUT must be greater than zero"

command -v curl >/dev/null 2>&1 || fail "curl is required"
command -v ssh >/dev/null 2>&1 || fail "ssh is required"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"

if [ -z "${token}" ]; then
    echo "Reading the OTA token from ${ssh_target}..."
    token=$(ssh "${ssh_target}" 'cat /data/ota/api-token') || fail "could not read the OTA token over SSH"
fi
token=$(printf '%s' "${token}" | tr '[:upper:]' '[:lower:]' | tr -d ':-')
case ${token} in
    *[!0-9a-f]*|'') fail "OTA token must contain 12 hexadecimal digits" ;;
esac
[ "${#token}" -eq 12 ] || fail "OTA token must contain 12 hexadecimal digits"

trap cleanup EXIT HUP INT TERM

echo "Opening an SSH tunnel through ${ssh_target} on local port ${local_port}..."
ssh -4 -N -o ExitOnForwardFailure=yes \
    -L "127.0.0.1:${local_port}:127.0.0.1:8080" \
    "${ssh_target}" &
tunnel_pid=$!

api_url="http://127.0.0.1:${local_port}/api/v1"
attempt=0
while ! curl --silent --show-error --fail "${api_url}/health" >/dev/null 2>&1; do
    if ! kill -0 "${tunnel_pid}" 2>/dev/null; then
        wait "${tunnel_pid}" || true
        fail "SSH tunnel exited before the OTA API became ready"
    fi
    attempt=$((attempt + 1))
    [ "${attempt}" -lt 15 ] || fail "timed out waiting for the OTA API"
    sleep 1
done

echo "Uploading $(basename -- "${bundle}")..."
curl --show-error --fail-with-body \
    -H "Authorization: Bearer ${token}" \
    -H "Content-Type: application/octet-stream" \
    --data-binary "@${bundle}" \
    "${api_url}/update"

echo "Waiting for RAUC installation to finish..."
started_at=$(date +%s)
while :; do
    status=$(curl --silent --show-error --fail \
        -H "Authorization: Bearer ${token}" \
        "${api_url}/update")
    state=$(printf '%s' "${status}" | python3 -c 'import json, sys; print(json.load(sys.stdin)["state"])') || fail "invalid OTA status response: ${status}"
    echo "OTA status: ${status}"

    case ${state} in
        succeeded) break ;;
        failed) fail "RAUC installation failed" ;;
        uploading|installing) ;;
        *) fail "unexpected OTA state: ${state}" ;;
    esac

    now=$(date +%s)
    [ $((now - started_at)) -lt "${poll_timeout}" ] || fail "timed out waiting for RAUC installation"
    sleep 2
done

echo "OTA installation succeeded."
if [ "${reboot}" = true ]; then
    echo "Requesting device reboot..."
    curl --show-error --fail-with-body -X POST \
        -H "Authorization: Bearer ${token}" \
        "${api_url}/reboot"
else
    echo "The device was not rebooted; use --reboot to reboot automatically after success."
fi
