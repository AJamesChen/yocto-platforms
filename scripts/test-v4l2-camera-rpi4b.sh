#!/bin/sh
set -eu

device=${1:-/dev/video0}
output=${2:-/tmp/v4l2-camera-test.jpg}
config=/boot/config.txt

fail() {
    echo "error: $*" >&2
    exit 1
}

[ "$(id -u)" -eq 0 ] || fail "run this test as root"
[ -r "${config}" ] || fail "cannot read ${config}"
command -v modprobe >/dev/null 2>&1 || fail "modprobe is not installed"
command -v v4l2-ctl >/dev/null 2>&1 || fail "v4l2-ctl is not installed"

grep -Eq '^start_x=1$' "${config}" || fail "start_x=1 is missing from ${config}"
grep -Eq '^gpu_mem=128$' "${config}" || fail "gpu_mem=128 is missing from ${config}"
grep -Eq '^camera_auto_detect=0$' "${config}" ||
    fail "camera_auto_detect=0 is missing from ${config}"

modprobe bcm2835-v4l2
[ -c "${device}" ] || fail "${device} was not created after loading bcm2835-v4l2"

echo "Detected V4L2 devices:"
v4l2-ctl --list-devices
echo
echo "Camera capabilities:"
v4l2-ctl --device="${device}" --all

v4l2-ctl --device="${device}" \
    --set-fmt-video=width=640,height=480,pixelformat=MJPG \
    --stream-mmap=3 --stream-count=1 --stream-to="${output}"

[ -s "${output}" ] || fail "capture completed but ${output} is empty"
bytes=$(wc -c < "${output}")
echo "Captured ${bytes} bytes to ${output}"
