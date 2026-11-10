#!/usr/bin/env python3
"""Serve dual CSI feeds with chessboard calibration and metric depth preview."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import argparse
import json
import logging
import os
from pathlib import Path
import tempfile
import threading
import time
from urllib.parse import parse_qs, urlsplit

import cv2
import numpy as np

WIDTH, HEIGHT, FPS = 640, 480, 15
NUM_DISPARITIES = 96
DEPTH_NEAR_M, DEPTH_FAR_M = 0.2, 5.0
CALIBRATION_PATH = Path("/data/stereo-camera-viewer/calibration.npz")

frame_lock = threading.Lock()
raw_lock = threading.Lock()
state_lock = threading.RLock()
frames = {"left": None, "right": None, "depth": None}
raw = {0: None, 1: None}
raw_time = {0: 0.0, 1: 0.0}
latest_depth = None
camera_status = {0: "starting", 1: "starting"}
state = {
    "columns": 9,
    "rows": 6,
    "square_mm": 25.0,
    "object_points": [],
    "left_points": [],
    "right_points": [],
    "calibration": None,
    "rms": None,
    "baseline_mm": None,
    "message": "Show a chessboard to both cameras and capture calibration views.",
    "calibrating": False,
}


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
            camera_status[sensor_id] = "Argus capture open failed"
            logging.error("Cannot open Argus sensor %d; retrying", sensor_id)
            cap.release()
            time.sleep(5)
            continue
        camera_status[sensor_id] = "capturing"
        logging.info("Capturing %s feed from sensor %d", channel, sensor_id)
        while True:
            ok, image = cap.read()
            if not ok:
                camera_status[sensor_id] = "frame read failed"
                logging.warning("Lost sensor %d; reopening", sensor_id)
                break
            encoded = encode(image)
            if encoded:
                with frame_lock:
                    frames[channel] = encoded
                with raw_lock:
                    raw[sensor_id] = image.copy()
                    raw_time[sensor_id] = time.monotonic()
        cap.release()
        time.sleep(2)


def detect_chessboard(image, columns, rows):
    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    pattern = (columns, rows)
    found, corners = cv2.findChessboardCorners(
        gray, pattern,
        cv2.CALIB_CB_ADAPTIVE_THRESH | cv2.CALIB_CB_NORMALIZE_IMAGE)
    if not found:
        return None
    criteria = (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_MAX_ITER,
                30, 0.001)
    return cv2.cornerSubPix(gray, corners, (11, 11), (-1, -1), criteria)


def take_calibration_sample():
    with raw_lock:
        left = None if raw[0] is None else raw[0].copy()
        right = None if raw[1] is None else raw[1].copy()
        left_time, right_time = raw_time[0], raw_time[1]
    if left is None or right is None:
        raise ValueError("Waiting for both camera streams.")
    skew_ms = abs(left_time - right_time) * 1000.0
    if skew_ms > 150:
        raise ValueError(f"Latest frames differ by {skew_ms:.0f} ms; hold the board still and retry.")

    with state_lock:
        columns, rows = state["columns"], state["rows"]
        square_mm = state["square_mm"]
    left_corners = detect_chessboard(left, columns, rows)
    right_corners = detect_chessboard(right, columns, rows)
    if left_corners is None or right_corners is None:
        raise ValueError("Chessboard not found in both feeds. Make all inner corners visible to both cameras.")

    points = np.zeros((rows * columns, 3), dtype=np.float32)
    points[:, :2] = np.mgrid[0:columns, 0:rows].T.reshape(-1, 2) * square_mm
    with state_lock:
        if state["calibrating"]:
            raise ValueError("Calibration is running; wait for it to finish.")
        state["object_points"].append(points)
        state["left_points"].append(left_corners)
        state["right_points"].append(right_corners)
        count = len(state["object_points"])
        state["message"] = f"Captured pair {count}; move or tilt the board before the next capture."
    return {"samples": count, "skew_ms": round(skew_ms, 1), "message": state["message"]}


def rectify_maps(calibration):
    size = (WIDTH, HEIGHT)
    map_lx, map_ly = cv2.initUndistortRectifyMap(
        calibration["K1"], calibration["D1"], calibration["R1"],
        calibration["P1"], size, cv2.CV_32FC1)
    map_rx, map_ry = cv2.initUndistortRectifyMap(
        calibration["K2"], calibration["D2"], calibration["R2"],
        calibration["P2"], size, cv2.CV_32FC1)
    calibration["maps"] = (map_lx, map_ly, map_rx, map_ry)
    return calibration


def save_calibration(calibration):
    CALIBRATION_PATH.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_path = tempfile.mkstemp(
        prefix="calibration-", suffix=".npz", dir=CALIBRATION_PATH.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            np.savez_compressed(
                output,
                image_size=np.array([WIDTH, HEIGHT], dtype=np.int32),
                K1=calibration["K1"], D1=calibration["D1"],
                K2=calibration["K2"], D2=calibration["D2"],
                R=calibration["R"], T=calibration["T"],
                R1=calibration["R1"], R2=calibration["R2"],
                P1=calibration["P1"], P2=calibration["P2"],
                Q=calibration["Q"], rms=np.array([calibration["rms"]]),
                baseline_mm=np.array([calibration["baseline_mm"]]),
                square_mm=np.array([calibration["square_mm"]]))
        os.replace(temp_path, CALIBRATION_PATH)
    finally:
        if os.path.exists(temp_path):
            os.unlink(temp_path)


def load_calibration():
    if not CALIBRATION_PATH.is_file():
        return None
    try:
        with np.load(CALIBRATION_PATH, allow_pickle=False) as stored:
            if tuple(stored["image_size"].tolist()) != (WIDTH, HEIGHT):
                raise ValueError("saved calibration resolution does not match camera stream")
            calibration = {key: stored[key].copy() for key in
                           ("K1", "D1", "K2", "D2", "R", "T", "R1", "R2", "P1", "P2", "Q")}
            calibration["rms"] = float(stored["rms"][0])
            calibration["baseline_mm"] = float(stored["baseline_mm"][0])
            calibration["square_mm"] = float(stored["square_mm"][0])
        return rectify_maps(calibration)
    except (OSError, KeyError, ValueError) as error:
        logging.error("Unable to load stereo calibration: %s", error)
        return None


def run_calibration():
    with state_lock:
        if state["calibrating"]:
            raise ValueError("Calibration is already running.")
        sample_count = len(state["object_points"])
        if sample_count < 12:
            raise ValueError(f"Capture at least 12 different board poses first; have {sample_count}.")
        state["calibrating"] = True
        state["message"] = "Calibrating cameras; this may take several seconds."
        object_points = [points.copy() for points in state["object_points"]]
        left_points = [points.copy() for points in state["left_points"]]
        right_points = [points.copy() for points in state["right_points"]]
        square_mm = state["square_mm"]

    try:
        size = (WIDTH, HEIGHT)
        criteria = (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_MAX_ITER,
                    100, 1e-6)
        rms_l, K1, D1, _, _ = cv2.calibrateCamera(
            object_points, left_points, size, None, None,
            criteria=criteria)
        rms_r, K2, D2, _, _ = cv2.calibrateCamera(
            object_points, right_points, size, None, None,
            criteria=criteria)
        stereo_rms, K1, D1, K2, D2, R, T, _, _ = cv2.stereoCalibrate(
            object_points, left_points, right_points, K1, D1, K2, D2,
            size, criteria=criteria, flags=cv2.CALIB_FIX_INTRINSIC)
        R1, R2, P1, P2, Q, _, _ = cv2.stereoRectify(
            K1, D1, K2, D2, size, R, T,
            flags=cv2.CALIB_ZERO_DISPARITY, alpha=0)
        baseline_mm = float(np.linalg.norm(T))
        if not np.isfinite(baseline_mm) or baseline_mm < 1.0:
            raise ValueError("calibration produced an invalid camera baseline")
        calibration = rectify_maps({
            "K1": K1, "D1": D1, "K2": K2, "D2": D2,
            "R": R, "T": T, "R1": R1, "R2": R2,
            "P1": P1, "P2": P2, "Q": Q,
            "rms": float(stereo_rms), "baseline_mm": baseline_mm,
            "square_mm": square_mm,
        })
        save_calibration(calibration)
        with state_lock:
            state["calibration"] = calibration
            state["rms"] = float(stereo_rms)
            state["baseline_mm"] = baseline_mm
            state["message"] = (f"Calibration saved. Stereo RMS {stereo_rms:.3f} px; "
                                 f"baseline {baseline_mm:.1f} mm "
                                 f"(camera RMS {rms_l:.3f}/{rms_r:.3f} px).")
            return status_snapshot()
    except cv2.error as error:
        raise ValueError(f"OpenCV calibration failed: {error}") from error
    finally:
        with state_lock:
            state["calibrating"] = False


def status_snapshot():
    with state_lock:
        calibration = state["calibration"]
        return {
            "columns": state["columns"],
            "rows": state["rows"],
            "square_mm": state["square_mm"],
            "samples": len(state["object_points"]),
            "required_samples": 12,
            "calibrated": calibration is not None,
            "rms": state["rms"],
            "baseline_mm": state["baseline_mm"],
            "message": state["message"],
            "calibrating": state["calibrating"],
            "camera_status": {"left": camera_status[0], "right": camera_status[1]},
            "depth_range_m": [DEPTH_NEAR_M, DEPTH_FAR_M],
        }


def colorize_relative_disparity(disparity):
    valid = np.isfinite(disparity) & (disparity > 0)
    view = np.zeros(disparity.shape, dtype=np.uint8)
    if np.any(valid):
        low, high = np.percentile(disparity[valid], (5, 95))
        if high <= low:
            high = low + 1.0
        view[valid] = np.clip((disparity[valid] - low) * 255.0 /
                              (high - low), 0, 255).astype(np.uint8)
    return cv2.applyColorMap(view, cv2.COLORMAP_TURBO)


def depth_image(left, right, calibration, matcher):
    map_lx, map_ly, map_rx, map_ry = calibration["maps"]
    left = cv2.remap(left, map_lx, map_ly, cv2.INTER_LINEAR)
    right = cv2.remap(right, map_rx, map_ry, cv2.INTER_LINEAR)
    gray_l = cv2.cvtColor(left, cv2.COLOR_BGR2GRAY)
    gray_r = cv2.cvtColor(right, cv2.COLOR_BGR2GRAY)
    disparity = matcher.compute(gray_l, gray_r).astype(np.float32) / 16.0
    xyz_mm = cv2.reprojectImageTo3D(disparity, calibration["Q"])
    depth_m = xyz_mm[:, :, 2] / 1000.0
    min_disparity = matcher.getMinDisparity()
    valid_disparity = ((disparity > min_disparity) &
                       (disparity < min_disparity + NUM_DISPARITIES))
    valid_depth = (valid_disparity & np.isfinite(depth_m) &
                   (depth_m >= DEPTH_NEAR_M) & (depth_m <= DEPTH_FAR_M))
    depth = np.full(depth_m.shape, np.nan, dtype=np.float32)
    depth[valid_depth] = depth_m[valid_depth]
    normalized = np.zeros(depth_m.shape, dtype=np.uint8)
    normalized[valid_depth] = np.clip(
        (DEPTH_FAR_M - depth_m[valid_depth]) * 255.0 /
        (DEPTH_FAR_M - DEPTH_NEAR_M), 0, 255).astype(np.uint8)
    view = cv2.applyColorMap(normalized, cv2.COLORMAP_TURBO)
    view[~valid_depth] = (0, 0, 0)
    cv2.putText(view, f"Depth {DEPTH_NEAR_M:.1f}-{DEPTH_FAR_M:.1f} m",
                (12, 26), cv2.FONT_HERSHEY_SIMPLEX, 0.65,
                (255, 255, 255), 2, cv2.LINE_AA)
    return view, depth


def disparity_worker():
    global latest_depth
    while True:
        with raw_lock:
            left = None if raw[0] is None else raw[0].copy()
            right = None if raw[1] is None else raw[1].copy()
            left_time, right_time = raw_time[0], raw_time[1]
        if left is None or right is None:
            time.sleep(0.2)
            continue
        if abs(left_time - right_time) > 0.15:
            time.sleep(0.05)
            continue

        with state_lock:
            calibration = state["calibration"]
        if calibration is None:
            left_gray = cv2.cvtColor(left, cv2.COLOR_BGR2GRAY)
            right_gray = cv2.cvtColor(right, cv2.COLOR_BGR2GRAY)
            min_disparity = 0
        else:
            # A negative X translation means the right camera is physically
            # to the right of the left camera and gives positive disparities.
            min_disparity = 0 if float(calibration["T"].reshape(-1)[0]) < 0 else -NUM_DISPARITIES
        matcher = cv2.StereoSGBM_create(
            minDisparity=min_disparity,
            numDisparities=NUM_DISPARITIES,
            blockSize=5,
            P1=600,
            P2=2400,
            disp12MaxDiff=1,
            uniquenessRatio=8,
            speckleWindowSize=50,
            speckleRange=2,
            mode=cv2.STEREO_SGBM_MODE_SGBM_3WAY)
        try:
            if calibration is None:
                disparity = matcher.compute(left_gray, right_gray).astype(np.float32) / 16.0
                view = colorize_relative_disparity(disparity)
                cv2.putText(view, "Relative disparity - calibrate for meters",
                            (12, 26), cv2.FONT_HERSHEY_SIMPLEX, 0.55,
                            (255, 255, 255), 2, cv2.LINE_AA)
                depth = None
            else:
                view, depth = depth_image(left, right, calibration, matcher)
            encoded = encode(view)
            if encoded:
                with frame_lock:
                    frames["depth"] = encoded
            with raw_lock:
                latest_depth = depth
        except cv2.error as error:
            logging.error("Stereo depth processing failed: %s", error)
            time.sleep(0.2)
        time.sleep(0.05)


PAGE = """<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Orin Nano stereo camera</title><style>
body{font:16px sans-serif;background:#15191f;color:#eee;margin:1rem}
main{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:1rem}
section{background:#242a33;padding:.7rem;border-radius:.5rem}img{width:100%;height:auto}
input{width:6rem;margin:.25rem}button{margin:.25rem;padding:.45rem}
.status{padding:.7rem;background:#303944;border-radius:.4rem;margin:.6rem 0}
</style></head><body><h1>Orin Nano stereo camera</h1>
<section><h2>Chessboard calibration</h2>
<p>Use a printed checkerboard with the configured number of inner corners. Measure one square accurately. Hold the board still and visible in both feeds; capture at least 12 views at different distances, tilts, and image positions.</p>
<label>Inner corners across <input id=columns type=number min=2 max=20 value=9></label>
<label>down <input id=rows type=number min=2 max=20 value=6></label>
<label>Square size (mm) <input id=square type=number min=1 max=500 step=0.1 value=25></label>
<button id=save-settings>Apply board settings</button>
<button id=capture>Capture board pair</button>
<button id=calibrate>Calibrate and save</button>
<button id=reset>Clear samples and calibration</button>
<div id=status class=status>Loading status…</div></section>
<main><section><h2>Left camera</h2><img src=/left.mjpg></section>
<section><h2>Right camera</h2><img src=/right.mjpg></section>
<section><h2>Depth preview</h2><img id=depth src=/depth.mjpg>
<p id=reading>After calibration, click a pixel to read its local median depth.</p>
<p>Near objects use warmer colors. Black means invalid or outside the displayed range.</p></section></main>
<script>
const statusBox=document.getElementById('status');
async function post(path,data={}){const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});const j=await r.json();if(!r.ok)throw new Error(j.error||'Request failed');return j;}
async function refresh(){try{const s=await (await fetch('/api/status')).json();document.getElementById('columns').value=s.columns;document.getElementById('rows').value=s.rows;document.getElementById('square').value=s.square_mm;const depth=s.calibrated?`Calibrated: RMS ${s.rms.toFixed(3)} px, baseline ${s.baseline_mm.toFixed(1)} mm`:'Not calibrated - preview is relative disparity';statusBox.textContent=`${depth}. Samples ${s.samples}/${s.required_samples}. ${s.message} Cameras: L ${s.camera_status.left}; R ${s.camera_status.right}.`;document.getElementById('calibrate').disabled=s.samples<s.required_samples||s.calibrating;}catch(e){statusBox.textContent=e.message;}}
function action(path,data){statusBox.textContent='Working…';post(path,data).then(s=>{statusBox.textContent=s.message||`Captured ${s.samples} pairs.`;refresh();}).catch(e=>{statusBox.textContent=e.message;});}
document.getElementById('save-settings').onclick=()=>action('/api/calibration/settings',{columns:+document.getElementById('columns').value,rows:+document.getElementById('rows').value,square_mm:+document.getElementById('square').value});
document.getElementById('capture').onclick=()=>action('/api/calibration/capture');
document.getElementById('calibrate').onclick=()=>action('/api/calibration/run');
document.getElementById('reset').onclick=()=>{if(confirm('Delete the saved calibration and captured samples?'))action('/api/calibration/reset');};
document.getElementById('depth').addEventListener('click',async e=>{const r=e.currentTarget.getBoundingClientRect(),x=Math.floor((e.clientX-r.left)*e.currentTarget.naturalWidth/r.width),y=Math.floor((e.clientY-r.top)*e.currentTarget.naturalHeight/r.height);try{const d=await (await fetch(`/api/depth?x=${x}&y=${y}`)).json();document.getElementById('reading').textContent=d.depth_m===null?`No valid depth at (${x}, ${y}).`:`Depth near (${x}, ${y}): ${d.depth_m.toFixed(3)} m (median of local valid pixels).`;}catch(_){document.getElementById('reading').textContent='Depth query failed.';}});
setInterval(refresh,2000);refresh();
</script></body></html>""".encode()


class Handler(BaseHTTPRequestHandler):
    def send_json(self, status, payload):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlsplit(self.path)
        if parsed.path in ("/", "/index.html"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(PAGE)))
            self.end_headers()
            self.wfile.write(PAGE)
            return
        if parsed.path == "/api/status":
            self.send_json(200, status_snapshot())
            return
        if parsed.path == "/api/depth":
            try:
                query = parse_qs(parsed.query)
                x, y = int(query["x"][0]), int(query["y"][0])
                with raw_lock:
                    depth = None if latest_depth is None else latest_depth.copy()
                if depth is None or not (0 <= x < WIDTH and 0 <= y < HEIGHT):
                    raise ValueError
                patch = depth[max(0, y - 2):min(HEIGHT, y + 3),
                              max(0, x - 2):min(WIDTH, x + 3)]
                valid = patch[np.isfinite(patch)]
                value = float(np.median(valid)) if valid.size else None
                self.send_json(200, {"x": x, "y": y, "depth_m": value})
            except (KeyError, IndexError, TypeError, ValueError):
                self.send_json(400, {"error": "invalid or unavailable depth coordinate"})
            return
        channel = {"/left.mjpg": "left", "/right.mjpg": "right", "/depth.mjpg": "depth"}.get(parsed.path)
        if channel is None:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.end_headers()
        try:
            while True:
                with frame_lock:
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

    def do_POST(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length > 8192:
                raise ValueError("request too large")
            payload = json.loads(self.rfile.read(length) or b"{}")
            if not isinstance(payload, dict):
                raise ValueError("request must be a JSON object")
            if self.path == "/api/calibration/settings":
                columns = int(payload["columns"])
                rows = int(payload["rows"])
                square_mm = float(payload["square_mm"])
                if not (2 <= columns <= 20 and 2 <= rows <= 20 and
                        1.0 <= square_mm <= 500.0):
                    raise ValueError("Use 2-20 inner corners and a square size from 1 to 500 mm.")
                with state_lock:
                    if state["calibrating"]:
                        raise ValueError("Calibration is running; wait for it to finish.")
                    state.update(columns=columns, rows=rows, square_mm=square_mm,
                                 object_points=[], left_points=[], right_points=[],
                                 calibration=None, rms=None, baseline_mm=None,
                                 message="Board settings updated; capture at least 12 varied poses.")
                    if CALIBRATION_PATH.exists():
                        CALIBRATION_PATH.unlink()
                self.send_json(200, status_snapshot())
                return
            if self.path == "/api/calibration/capture":
                self.send_json(200, take_calibration_sample())
                return
            if self.path == "/api/calibration/run":
                self.send_json(200, run_calibration())
                return
            if self.path == "/api/calibration/reset":
                with state_lock:
                    if state["calibrating"]:
                        raise ValueError("Calibration is running; wait for it to finish.")
                    state.update(object_points=[], left_points=[], right_points=[],
                                 calibration=None, rms=None, baseline_mm=None,
                                 message="Samples and saved calibration cleared.")
                    if CALIBRATION_PATH.exists():
                        CALIBRATION_PATH.unlink()
                with raw_lock:
                    global latest_depth
                    latest_depth = None
                self.send_json(200, status_snapshot())
                return
            self.send_json(404, {"error": "unknown API endpoint"})
        except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
            self.send_json(400, {"error": str(error)})
        except OSError as error:
            logging.exception("Calibration storage operation failed")
            self.send_json(500, {"error": f"calibration storage failed: {error}"})

    def log_message(self, fmt, *args):
        logging.info("%s - %s", self.address_string(), fmt % args)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    calibration = load_calibration()
    with state_lock:
        state["calibration"] = calibration
        if calibration is not None:
            state["rms"] = calibration["rms"]
            state["baseline_mm"] = calibration["baseline_mm"]
            state["square_mm"] = calibration["square_mm"]
            state["message"] = "Loaded saved calibration from /data."
    frames.update(left=placeholder("Waiting for left camera"),
                  right=placeholder("Waiting for right camera"),
                  depth=placeholder("Waiting for both cameras"))
    threading.Thread(target=capture, args=(0, "left"), daemon=True).start()
    threading.Thread(target=capture, args=(1, "right"), daemon=True).start()
    threading.Thread(target=disparity_worker, daemon=True).start()
    logging.info("Calibration file: %s", CALIBRATION_PATH)
    logging.info("Open http://<device-ip>:%d/ to view stereo and depth", args.port)
    ThreadingHTTPServer((args.bind, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
