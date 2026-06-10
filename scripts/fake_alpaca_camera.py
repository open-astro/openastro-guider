#!/usr/bin/env python3
"""Minimal fake ASCOM Alpaca camera server for testing the guider without hardware.

Serves a single camera at device number 0 with a deterministic synthetic star
field (noisy background + a dozen Gaussian stars), enough for the guider's
camera connect path, capture_single_frame, find_star, and get_star_centroids.

With --rotate-deg-per-sec the star field rotates about --pole (a fake celestial
pole on the sensor), which is what the polar-alignment flows need: the static-PA
circle fit should recover the pole point as its centre of rotation.

A fake Alpaca *telescope* is served at device 0 too. Its pulseguide requests
shift the star field (so the guider can calibrate and guide against it), and
--dec-drift-px-per-min injects a steady dec-axis drift simulating polar
misalignment - what the drift-align flow measures. Slew endpoints update the
reported coordinates only.

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

# Telescope simulation: pulses shift the star field; optional dec drift
GUIDE_RATE_DEG_S = 15.0 / 3600.0  # 1x sidereal
IMAGE_SCALE = 7.73  # arcsec/px the pulses are converted with
DEC_DRIFT_PX_PER_MIN = 0.0
scope = {"connected": False, "ra": 12.0, "dec": 30.0, "pulse_x": 0.0, "pulse_y": 0.0}

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
    # pulses + injected dec drift shift the whole field
    ox = scope["pulse_x"]
    oy = scope["pulse_y"] + DEC_DRIFT_PX_PER_MIN * (time.monotonic() - T0) / 60.0
    yy, xx = np.mgrid[0:H, 0:W]
    for sx, sy, flux in STARS:
        rx, ry = sx - px, sy - py
        cx = px + rx * math.cos(th) - ry * math.sin(th) + ox
        cy = py + rx * math.sin(th) + ry * math.cos(th) + oy
        img += flux * np.exp(-(((xx - cx) ** 2 + (yy - cy) ** 2) / (2 * 2.0 ** 2)))
    img = img.clip(0, 65535).astype(int)
    # Alpaca imagearray is [x][y] (column-major)
    return img.T.tolist()


def star_field():
    """Synthetic frame: noisy background + a dozen Gaussian stars, optionally
    rotated about the fake pole by the elapsed-time angle."""
    global _frame
    with state_lock:
        if ROTATE_DEG_PER_SEC != 0.0 or DEC_DRIFT_PX_PER_MIN != 0.0 or scope["pulse_x"] or scope["pulse_y"]:
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


SCOPE_PROPS = {
    "name": "FakeScope",
    "description": "Synthetic Alpaca telescope for tests",
    "interfaceversion": 3,
    "canpulseguide": True,
    "canslew": True,
    "canslewasync": True,
    "ispulseguiding": False,
    "slewing": False,
    "sideofpier": 0,
    "guideratedeclination": GUIDE_RATE_DEG_S,
    "guideraterightascension": GUIDE_RATE_DEG_S,
    "sitelatitude": 45.0,
    "sitelongitude": -110.0,
}


def lst_hours():
    # crude advancing sidereal clock; absolute value is irrelevant for tests
    return (12.0 + (time.monotonic() - T0) * 1.0027 / 3600.0) % 24.0


def apply_pulse(direction, duration_ms):
    """Alpaca GuideDirections: 0=N 1=S 2=E 3=W. Shift the field accordingly."""
    px = GUIDE_RATE_DEG_S * (duration_ms / 1000.0) * 3600.0 / IMAGE_SCALE
    with state_lock:
        if direction == 0:
            scope["pulse_y"] -= px
        elif direction == 1:
            scope["pulse_y"] += px
        elif direction == 2:
            scope["pulse_x"] += px
        elif direction == 3:
            scope["pulse_x"] -= px


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

    def scope_prop_name(self):
        m = re.match(r"^/api/v1/telescope/0/(\w+)", self.path)
        return m.group(1) if m else None

    def do_GET(self):
        sprop = self.scope_prop_name()
        if sprop is not None:
            if sprop == "connected":
                with state_lock:
                    return self.reply(scope["connected"])
            if sprop == "rightascension":
                return self.reply(scope["ra"])
            if sprop == "declination":
                return self.reply(scope["dec"])
            if sprop == "siderealtime":
                return self.reply(lst_hours())
            if sprop in SCOPE_PROPS:
                return self.reply(SCOPE_PROPS[sprop])
            self.log_message("unknown scope GET %s", self.path)
            return self.reply(0)

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

        sprop = self.scope_prop_name()
        if sprop is not None:
            params = dict(p.split("=", 1) for p in body.split("&") if "=" in p)
            if sprop == "connected":
                with state_lock:
                    scope["connected"] = "true" in body.lower()
            elif sprop == "pulseguide":
                apply_pulse(int(params.get("Direction", -1)), float(params.get("Duration", 0)))
            elif sprop in ("slewtocoordinates", "slewtocoordinatesasync"):
                with state_lock:
                    scope["ra"] = float(params.get("RightAscension", scope["ra"]))
                    scope["dec"] = float(params.get("Declination", scope["dec"]))
            elif sprop == "abortslew":
                pass
            else:
                self.log_message("unknown scope PUT %s body=%s", self.path, body)
            return self.reply(None)

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
    ap.add_argument("--dec-drift-px-per-min", type=float, default=0.0,
                    help="steady dec-axis (y) star drift simulating polar misalignment")
    ap.add_argument("--image-scale", type=float, default=7.73,
                    help="arcsec/px used to convert telescope pulses into star-field shifts")
    args = ap.parse_args()
    ROTATE_DEG_PER_SEC = args.rotate_deg_per_sec
    POLE = tuple(float(v) for v in args.pole.split(","))
    DEC_DRIFT_PX_PER_MIN = args.dec_drift_px_per_min
    IMAGE_SCALE = args.image_scale
    print(f"fake-alpaca: serving camera+telescope 0 on {args.host}:{args.port}"
          f" (rotation {ROTATE_DEG_PER_SEC} deg/s about {POLE}, dec drift {DEC_DRIFT_PX_PER_MIN} px/min)", flush=True)
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()
