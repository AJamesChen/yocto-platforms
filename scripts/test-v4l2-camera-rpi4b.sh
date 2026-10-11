#!/bin/sh
set -eu

device=${1:-/dev/video0}
output=${2:-/tmp/v4l2-camera-test.jpg}
gst_output=${3:-/tmp/gstreamer-camera-test.jpg}
gst_osd_output=${4:-/tmp/gstreamer-camera-osd-test.jpg}
config=/boot/config.txt
restart_device_web=false

fail() {
    echo "error: $*" >&2
    exit 1
}

cleanup() {
    if [ "${restart_device_web}" = true ]; then
        systemctl start device-web.service
    fi
}

trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run this test as root"
[ -r "${config}" ] || fail "cannot read ${config}"
command -v modprobe >/dev/null 2>&1 || fail "modprobe is not installed"
command -v v4l2-ctl >/dev/null 2>&1 || fail "v4l2-ctl is not installed"
command -v gst-launch-1.0 >/dev/null 2>&1 || fail "gst-launch-1.0 is not installed"
command -v gst-inspect-1.0 >/dev/null 2>&1 || fail "gst-inspect-1.0 is not installed"
gst-inspect-1.0 v4l2src >/dev/null 2>&1 || fail "GStreamer v4l2src is not installed"
for element in jpegdec videoconvert textoverlay jpegenc; do
    gst-inspect-1.0 "${element}" >/dev/null 2>&1 || fail "GStreamer ${element} is not installed"
done

if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet device-web.service; then
    echo "Temporarily stopping device-web.service for exclusive camera access..."
    systemctl stop device-web.service
    restart_device_web=true
fi

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

gst-launch-1.0 -q \
    v4l2src device="${device}" io-mode=mmap num-buffers=1 \
    ! "image/jpeg,width=640,height=480,framerate=15/1" \
    ! filesink location="${gst_output}"

[ -s "${gst_output}" ] || fail "GStreamer capture completed but ${gst_output} is empty"
gst_bytes=$(wc -c < "${gst_output}")
echo "GStreamer captured ${gst_bytes} bytes to ${gst_output}"

gst-launch-1.0 -q \
    v4l2src device="${device}" io-mode=mmap num-buffers=1 \
    ! "image/jpeg,width=640,height=480,framerate=15/1" \
    ! jpegdec ! videoconvert \
    ! textoverlay text=MaxTV halignment=right valignment=top \
        "font-desc=Sans 20" xpad=12 ypad=12 draw-shadow=true draw-outline=true \
    ! jpegenc quality=90 ! filesink location="${gst_osd_output}"

[ -s "${gst_osd_output}" ] || fail "GStreamer OSD capture completed but ${gst_osd_output} is empty"
gst_osd_bytes=$(wc -c < "${gst_osd_output}")
echo "GStreamer OSD captured ${gst_osd_bytes} bytes to ${gst_osd_output}"
