#!/usr/bin/env python3
"""Minimal fake ASCOM Alpaca camera server for testing the guider without hardware.

Serves a single camera at device number 0 with a deterministic synthetic star
field (noisy background + a dozen Gaussian stars), enough for the guider's
camera connect path, capture_single_frame, find_star, and get_star_centroids.

With --rotate-deg-per-sec the star field rotates about --pole (a fake celestial
pole on the sensor), which is what the polar-alignment flows need: the static-PA
circle fit should recover the pole point as its centre of rotation.

Usage:
    python3 scripts/fake_alpaca_camera.py [--host 127.0.0.1] [--port 11111] \
        [--rotate-deg-per-sec 0.8] [--pole 200,150]

Then point the guider at it:
    {"method":"set_alpaca_server","params":{"host":"127.0.0.1","port":11111,"camera_device":0}}
    {"method":"set_selected_camera","params":{"camera":"Alpaca Camera [127.0.0.1:11111/0]"}}
    {"method":"set_connected","params":{"connected":true}}

Requires numpy (frame synthesis only).
"""
import argparse
import json
import math
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

W, H = 640, 480
state = {"connected": False, "imageready": False}
# handlers run on concurrent threads (ThreadingHTTPServer); guard shared state
state_lock = threading.Lock()

# Star-field rotation about a fake on-sensor pole (off by default; see --rotate-deg-per-sec)
ROTATE_DEG_PER_SEC = 0.0
POLE = (200.0, 150.0)
T0 = time.monotonic()

STARS = [(80, 60, 30000), (200, 120, 25000), (320, 240, 40000), (500, 100, 20000),
         (120, 350, 35000), (420, 380, 28000), (560, 300, 22000), (250, 420, 18000),
         (380, 60, 26000), (60, 250, 32000), (520, 430, 24000), (300, 160, 15000)]

_frame = None  # cache used only when rotation is off


def render(theta_deg):
    import numpy as np

    rng = np.random.default_rng(7)
    img = rng.normal(800, 15, (H, W))
    th = math.radians(theta_deg)
    px, py = POLE
    yy, xx = np.mgrid[0:H, 0:W]
    for sx, sy, flux in STARS:
        rx, ry = sx - px, sy - py
        cx = px + rx * math.cos(th) - ry * math.sin(th)
        cy = py + rx * math.sin(th) + ry * math.cos(th)
        img += flux * np.exp(-(((xx - cx) ** 2 + (yy - cy) ** 2) / (2 * 2.0 ** 2)))
    img = img.clip(0, 65535).astype(int)
    # Alpaca imagearray is [x][y] (column-major)
    return img.T.tolist()


def star_field():
    """Synthetic frame: noisy background + a dozen Gaussian stars, optionally
    rotated about the fake pole by the elapsed-time angle."""
    global _frame
    with state_lock:
        if ROTATE_DEG_PER_SEC != 0.0:
            return render(ROTATE_DEG_PER_SEC * (time.monotonic() - T0))
        if _frame is None:
            _frame = render(0.0)
        return _frame


PROPS = {
    "name": "FakeCam",
    "description": "Synthetic Alpaca camera for tests",
    "interfaceversion": 3,
    "camerastate": 0,
    "cameraxsize": W,
    "cameraysize": H,
    "canabortexposure": True,
    "canstopexposure": True,
    "canpulseguide": False,
    "cangetcoolerpower": False,
    "cansetccdtemperature": False,
    "ccdtemperature": 10.0,
    "cooleron": False,
    "coolerpower": 0.0,
    "hasshutter": False,
    "maxadu": 65535,
    "maxbinx": 1,
    "maxbiny": 1,
    "binx": 1,
    "biny": 1,
    "numx": W,
    "numy": H,
    "startx": 0,
    "starty": 0,
    "pixelsizex": 3.75,
    "pixelsizey": 3.75,
    "sensortype": 0,
}


class Handler(BaseHTTPRequestHandler):
    def reply(self, value):
        body = json.dumps({"Value": value, "ErrorNumber": 0, "ErrorMessage": ""}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def prop_name(self):
        m = re.match(r"^/api/v1/camera/0/(\w+)", self.path)
        return m.group(1) if m else None

    def do_GET(self):
        prop = self.prop_name()
        if prop == "connected":
            with state_lock:
                value = state["connected"]
            return self.reply(value)
        if prop == "imageready":
            with state_lock:
                value = state["imageready"]
            return self.reply(value)
        if prop == "imagearray":
            return self.reply(star_field())
        if prop in PROPS:
            return self.reply(PROPS[prop])
        self.log_message("unknown GET %s", self.path)
        return self.reply(0)

    def do_PUT(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length).decode() if length else ""
        prop = self.prop_name()
        if prop == "connected":
            with state_lock:
                state["connected"] = "true" in body.lower()
        elif prop == "startexposure":
            with state_lock:
                state["imageready"] = True
        elif prop in ("abortexposure", "stopexposure"):
            with state_lock:
                state["imageready"] = False
        else:
            self.log_message("unknown PUT %s body=%s", self.path, body)
        return self.reply(None)

    def log_message(self, fmt, *args):
        print("fake-alpaca:", fmt % args, flush=True)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=11111)
    ap.add_argument("--rotate-deg-per-sec", type=float, default=0.0,
                    help="rotate the star field about --pole at this rate (simulates RA rotation)")
    ap.add_argument("--pole", default="200,150", help="fake on-sensor pole x,y for --rotate-deg-per-sec")
    args = ap.parse_args()
    ROTATE_DEG_PER_SEC = args.rotate_deg_per_sec
    POLE = tuple(float(v) for v in args.pole.split(","))
    print(f"fake-alpaca: serving camera 0 on {args.host}:{args.port}"
          f" (rotation {ROTATE_DEG_PER_SEC} deg/s about {POLE})", flush=True)
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()
