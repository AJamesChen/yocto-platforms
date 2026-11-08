#!/usr/bin/env python3
"""Serve dual CSI feeds and an uncalibrated stereo disparity preview."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import argparse
import logging
import threading
import time

import cv2
import numpy as np

WIDTH, HEIGHT, FPS = 640, 480, 15
lock = threading.Lock()
raw_lock = threading.Lock()
frames = {"left": None, "right": None, "depth": None}
raw = {0: None, 1: None}


def encode(image):
    ok, data = cv2.imencode(".jpg", image, [cv2.IMWRITE_JPEG_QUALITY, 82])
    return data.tobytes() if ok else None


def placeholder(text):
    image = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
    cv2.putText(image, text, (20, HEIGHT // 2), cv2.FONT_HERSHEY_SIMPLEX,
                0.8, (255, 255, 255), 2, cv2.LINE_AA)
    return encode(image)


def pipeline(sensor_id):
    return (f"nvarguscamerasrc sensor-id={sensor_id} ! "
            f"video/x-raw(memory:NVMM),width={WIDTH},height={HEIGHT},framerate={FPS}/1 ! "
            "nvvidconv ! video/x-raw,format=BGRx ! videoconvert ! "
            "video/x-raw,format=BGR ! appsink drop=true max-buffers=1 sync=false")


def capture(sensor_id, channel):
    while True:
        cap = cv2.VideoCapture(pipeline(sensor_id), cv2.CAP_GSTREAMER)
        if not cap.isOpened():
            logging.error("Cannot open Argus sensor %d; retrying", sensor_id)
            cap.release()
            time.sleep(5)
            continue
        logging.info("Capturing %s feed from sensor %d", channel, sensor_id)
        while True:
            ok, image = cap.read()
            if not ok:
                logging.warning("Lost sensor %d; reopening", sensor_id)
                break
            encoded = encode(image)
            if encoded:
                with lock:
                    frames[channel] = encoded
                with raw_lock:
                    raw[sensor_id] = image
        cap.release()
        time.sleep(2)


def disparity_worker():
    matcher = cv2.StereoSGBM_create(
        minDisparity=0, numDisparities=96, blockSize=5,
        P1=600, P2=2400, disp12MaxDiff=1, uniquenessRatio=8,
        speckleWindowSize=50, speckleRange=2,
        mode=cv2.STEREO_SGBM_MODE_SGBM_3WAY)
    while True:
        with raw_lock:
            left = None if raw[0] is None else raw[0].copy()
            right = None if raw[1] is None else raw[1].copy()
        if left is None or right is None:
            time.sleep(0.2)
            continue
        left = cv2.cvtColor(left, cv2.COLOR_BGR2GRAY)
        right = cv2.cvtColor(right, cv2.COLOR_BGR2GRAY)
        disp = matcher.compute(left, right).astype(np.float32) / 16.0
        valid = disp > 0
        view = np.zeros(disp.shape, dtype=np.uint8)
        if np.any(valid):
            view[valid] = np.clip(disp[valid] * 255.0 / 96.0, 0, 255).astype(np.uint8)
        encoded = encode(cv2.applyColorMap(view, cv2.COLORMAP_TURBO))
        if encoded:
            with lock:
                frames["depth"] = encoded
        time.sleep(0.05)


PAGE = b"""<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Orin Nano stereo camera</title><style>
body{font:16px sans-serif;background:#15191f;color:#eee;margin:1rem}
main{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:1rem}
section{background:#242a33;padding:.7rem;border-radius:.5rem}img{width:100%;height:auto}
</style></head><body><h1>Orin Nano dual camera</h1><main>
<section><h2>Left camera</h2><img src=/left.mjpg></section>
<section><h2>Right camera</h2><img src=/right.mjpg></section>
<section><h2>Disparity preview</h2><img src=/depth.mjpg>
<p>Uncalibrated disparity: relative depth only, not metric distance. Reliable depth
requires calibration, rectification, and synchronized camera frames.</p></section>
</main></body></html>"""


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(PAGE)))
            self.end_headers()
            self.wfile.write(PAGE)
            return
        channel = {"/left.mjpg": "left", "/right.mjpg": "right", "/depth.mjpg": "depth"}.get(self.path)
        if channel is None:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.end_headers()
        try:
            while True:
                with lock:
                    image = frames[channel]
                if image is None:
                    image = placeholder("Waiting for camera frames")
                header = (b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                          + str(len(image)).encode() + b"\r\n\r\n")
                self.wfile.write(header + image + b"\r\n")
                self.wfile.flush()
                time.sleep(1.0 / FPS)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, fmt, *args):
        logging.info("%s - %s", self.address_string(), fmt % args)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    frames.update(left=placeholder("Waiting for left camera"),
                  right=placeholder("Waiting for right camera"),
                  depth=placeholder("Waiting for both cameras"))
    threading.Thread(target=capture, args=(0, "left"), daemon=True).start()
    threading.Thread(target=capture, args=(1, "right"), daemon=True).start()
    threading.Thread(target=disparity_worker, daemon=True).start()
    logging.info("Open http://<device-ip>:%d/ to view the streams", args.port)
    ThreadingHTTPServer((args.bind, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
