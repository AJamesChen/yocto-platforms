#!/usr/bin/env python3
"""Exercise a DT-bound motor and print commanded/measured step rates."""
import argparse
import pathlib
import signal
import sys
import time


def bounded(low, high):
    def parse(value):
        number = int(value)
        if not low <= number <= high:
            raise argparse.ArgumentTypeError(f"must be {low}..{high}")
        return number
    return parse


def status(device):
    return dict(field.split("=", 1)
                for field in (device / "status").read_text().split())


def move(device, direction, speed, steps, steps_per_rev):
    started = False
    try:
        (device / "move").write_text(f"{direction} {speed} {steps}\n")
        started = True
        begin = time.monotonic()
        deadline = begin + steps / speed * 4 + 5
        print(f"{'clockwise' if direction == 'cw' else 'anticlockwise'}: "
              f"speed={speed} half-steps/s, "
              f"nominal={speed * 60 / steps_per_rev:.2f} RPM", flush=True)
        while True:
            state = status(device)
            elapsed = max(time.monotonic() - begin, 0.001)
            print(f"  {state['direction']} speed={state['speed']} half-steps/s "
                  f"completed={state['completed']} remaining={state['remaining']} "
                  f"observed={int(state['completed']) / elapsed:.1f} half-steps/s",
                  flush=True)
            if state["running"] == "0":
                if int(state["completed"]) != steps:
                    raise RuntimeError("move stopped before all steps completed")
                return
            if time.monotonic() > deadline:
                raise RuntimeError("motor completion timed out")
            time.sleep(0.5)
    finally:
        if started:
            (device / "stop").write_text("1\n")


def interrupted(signum, frame):
    raise KeyboardInterrupt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", type=pathlib.Path,
                        help="platform device directory containing move/status/stop")
    parser.add_argument("--direction", choices=("cw", "acw", "both"), default="both")
    parser.add_argument("--speed", type=bounded(1, 1000), nargs="+", default=[128, 256],
                        help="one or more half-step rates (default: 128 256)")
    parser.add_argument("--steps", type=bounded(1, 1000000), default=512)
    parser.add_argument("--steps-per-rev", type=bounded(1, 1000000), default=4096,
                        help="nominal motor half-steps/revolution; calibrate for your motor")
    args = parser.parse_args()
    if args.device is None:
        devices = sorted(pathlib.Path("/sys/bus/platform/drivers/uln2003a").glob("*/move"))
        if len(devices) != 1:
            parser.error("expected one bound motor; load uln2003a/overlay or use --device")
        args.device = devices[0].parent
    signal.signal(signal.SIGTERM, interrupted)
    directions = ("cw", "acw") if args.direction == "both" else (args.direction,)
    try:
        for speed in args.speed:
            for direction in directions:
                move(args.device, direction, speed, args.steps, args.steps_per_rev)
        print("PASS: all selected moves completed; coils off.")
        print("Check the shaft visually: software cannot detect stalls or actual RPM.")
    except KeyboardInterrupt:
        print("Stopped; coils released.", file=sys.stderr)
        return 130
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
