#!/usr/bin/env python3
"""Minimal fake ASCOM Alpaca camera server for testing the guider without hardware.

Serves a single camera at device number 0 with a deterministic synthetic star
field (noisy background + a dozen Gaussian stars), enough for the guider's
camera connect path, capture_single_frame, find_star, and get_star_centroids.

Usage:
    python3 scripts/fake_alpaca_camera.py [--host 127.0.0.1] [--port 11111]

Then point the guider at it:
    {"method":"set_alpaca_server","params":{"host":"127.0.0.1","port":11111,"camera_device":0}}
    {"method":"set_selected_camera","params":{"camera":"Alpaca Camera [127.0.0.1:11111/0]"}}
    {"method":"set_connected","params":{"connected":true}}

Requires numpy (frame synthesis only).
"""
import argparse
import json
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

W, H = 640, 480
state = {"connected": False, "imageready": False}

_frame = None


def star_field():
    """Synthetic frame: noisy background + a dozen Gaussian stars."""
    global _frame
    if _frame is None:
        import numpy as np

        rng = np.random.default_rng(7)
        img = rng.normal(800, 15, (H, W))
        stars = [(80, 60, 30000), (200, 120, 25000), (320, 240, 40000), (500, 100, 20000),
                 (120, 350, 35000), (420, 380, 28000), (560, 300, 22000), (250, 420, 18000),
                 (380, 60, 26000), (60, 250, 32000), (520, 430, 24000), (300, 160, 15000)]
        yy, xx = np.mgrid[0:H, 0:W]
        for sx, sy, flux in stars:
            img += flux * np.exp(-(((xx - sx) ** 2 + (yy - sy) ** 2) / (2 * 2.0 ** 2)))
        img = img.clip(0, 65535).astype(int)
        # Alpaca imagearray is [x][y] (column-major)
        _frame = img.T.tolist()
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
            return self.reply(state["connected"])
        if prop == "imageready":
            return self.reply(state["imageready"])
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
            state["connected"] = "true" in body.lower()
        elif prop == "startexposure":
            state["imageready"] = True
        elif prop in ("abortexposure", "stopexposure"):
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
    args = ap.parse_args()
    print(f"fake-alpaca: serving camera 0 on {args.host}:{args.port}", flush=True)
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()
