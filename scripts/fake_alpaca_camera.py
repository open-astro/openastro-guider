#!/usr/bin/env python3
"""Minimal fake ASCOM Alpaca camera server for testing the guider without hardware.

Serves a single camera at device number 0 with a deterministic synthetic star
field (noisy background + a dozen Gaussian stars), enough for the guider's
camera connect path, capture_single_frame, find_star, and get_star_centroids.

With --rotate-deg-per-sec the star field rotates about --pole (a fake celestial
pole on the sensor), which is what RA rotation does to the sky: a circle fit
through the star's positions should recover the pole point as its centre.

A fake Alpaca *telescope* is served at device 0 too. Its pulseguide requests
shift the star field (so the guider can calibrate and guide against it), and
--dec-drift-px-per-min injects a steady dec-axis drift simulating polar
misalignment - what the drift-align flow measures. Slew endpoints update the
reported coordinates only, unless --slew-seconds is given:

    --slew-seconds S   slewtocoordinatesasync takes S seconds: telescope `slewing`
                       reads true and the reported RA moves from the start to the
                       target until S has passed; abortslew stops it where it is.
    --slew-rotates     the field angle follows the scope's RA about --pole,
                       (RA at start - RA now) * 15 deg, interpolated during a slew,
                       which is what any RA slew does to the sky.
    --slew-smear F     a frame whose exposure (startexposure time + Duration)
                       overlapped a slew is drawn with the stars at the field angle
                       of the middle of the overlap and their flux divided by F
                       (about 80 on the #98 rig), like a star smeared by the slew.

    e.g. RA slews about a pole 5 deg off the star at 7.73"/px:
        --slew-seconds 2 --slew-rotates --slew-smear 80 --pole 320,-2089

Larger sensors and denser fields (#99):

    --size W,H         sensor size (default 640,480)
    --stars N          N random stars per frame area instead of the dozen fixed ones,
                       spread over twice the frame each way so a shifted field brings
                       new stars in; --star-seed picks the field (default 1)
    --imagebytes       answer imagearray with ASCOM ImageBytes when the client accepts
                       it (JSON otherwise): a 1280x960 frame is 2.5 MB instead of ~7 MB
                       of JSON

    PUT /fake/shift    dx=<px>&dy=<px>[&absolute=true]
        Move the whole star field (a bolt turn): added to the current offset, or set
        with absolute=true. Applied after rotation, like the pulse offsets.
    PUT /fake/decoy    x=<px>&y=<px>[&flux=<peak ADU, default 30000>] | clear=true
        Add a star fixed on the sensor that does not move with the field (for the
        "latched onto another star" case); clear=true removes them all.

A camera readout (#120, the guider's ImageReady poll backoff):

    --readout-ms N     imageready reads false until N ms after the exposure is due
                       (startexposure time + Duration + N), and the server logs how
                       many imageready polls each frame took once it reads true.
                       Default 0: imageready reads true as soon as startexposure
                       arrives, as before.

Error injection (for the guider's capture-error handling, #103) goes through a
small control endpoint and is off until used, so the default behaviour is unchanged:

    PUT /fake/inject   call=<binx|biny|gain|startexposure|imageready|imagearray|cameraxsize>
                       &error=<Alpaca ErrorNumber, e.g. 1031 or 1280>&count=<N, default 1>
                       [&message=<ErrorMessage>][&format=<json|imagebytes>][&http=<status>]
        The next N calls of that camera endpoint answer with the Alpaca error
        (HTTP 200). imagearray errors are JSON by default; format=imagebytes sends
        an ImageBytes error payload instead. http=<status> (e.g. 400 or 500) answers
        with that HTTP status and the message as a text/plain body instead of an
        Alpaca error (#97). A new inject for a call replaces the old one; count=0
        clears it. The camera has gain control (gainmin 0, gainmax 100), so the
        guider writes gain when it changes; a gain failure must not fail the frame.
        cameraxsize fails a connect after its connected check, with the camera left
        connected on the server (#104: a driver failure the probe can't see).
    PUT /fake/unplug   unplugged=<true|false>[&device=<camera|telescope>][&message=<ErrorMessage>]
                       [&error=<ErrorNumber>][&delay_ms=<N>][&connect_delay_ms=<N>]
        device=camera (the default): while unplugged, the camera reads as not
        connected, a Connected=true PUT is refused with 1031, and binx/biny/gain/
        startexposure/imageready/imagearray answer 1031 (an injection for the call
        still wins).
        device=telescope (#104): while unplugged, the telescope reads as not
        connected and a Connected=true PUT is refused with error (default 1280) and
        message (default a Sky-Watcher "did not reply" text) after delay_ms (default
        0; ~2300 reproduces AlpacaBridge timing out on a powered-off mount).
        unplugged=false lets the next Connected=true PUT succeed. connect_delay_ms
        (telescope, default 0) holds a Connected=true PUT that succeeds that long
        before it answers, connected only then (a synchronous connect that opens a
        serial port).
    PUT /fake/stall    device=<camera|telescope>&ms=<N>
        Every request for that device sleeps N ms before it answers (a server that
        accepts the connection, then stalls); ms=0 clears it.
    GET /fake/state    the injections, the unplugged flags and the stalls, as JSON.

Setup checks (#117: set_profile_setup must reach the live mount and camera objects):

    GET /fake/pulses[?clear=1]
        The telescope pulseguide requests since the last clear (the latest 1000), as
        {"pulses": [{"direction": 0-3 (N S E W), "duration": ms}, ...]}; clear=1
        returns them and empties the list.

    e.g. curl -X PUT -d 'call=imageready&error=1280&count=3&message=acq failed' \
             http://127.0.0.1:11111/fake/inject

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
import collections
import json
import math
import re
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs

W, H = 640, 480
IMAGEBYTES = False
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
# the pulseguide requests received, latest last (see GET /fake/pulses)
pulses = collections.deque(maxlen=1000)

# Slew simulation (off by default; see --slew-seconds / --slew-rotates / --slew-smear)
SLEW_SECONDS = 0.0
SLEW_ROTATES = False
SLEW_SMEAR = 0.0
RA0 = scope["ra"]
# the slew in progress or last finished: monotonic start/end, RA from/to
slew = {"t0": 0.0, "t1": 0.0, "ra0": 0.0, "ra1": 0.0}
# the last startexposure: monotonic start and Duration (s), and the imageready
# polls since (logged once it reads true with --readout-ms)
exposure = {"t0": 0.0, "dur": 0.0, "polls": 0, "logged": False}
# --readout-ms: imageready reads false until this long after the exposure is due
READOUT_MS = 0.0

STARS = [(80, 60, 30000), (200, 120, 25000), (320, 240, 40000), (500, 100, 20000),
         (120, 350, 35000), (420, 380, 28000), (560, 300, 22000), (250, 420, 18000),
         (380, 60, 26000), (60, 250, 32000), (520, 430, 24000), (300, 160, 15000)]

_frame = None  # cache used only when rotation is off

# Field translation (bolt turns) and sensor-fixed decoy stars (see /fake/shift, /fake/decoy)
shift = {"x": 0.0, "y": 0.0}
decoys = []

# Error injection (see the module docstring): call -> {"error", "message", "count", "format"}
INJECTABLE_CALLS = ("binx", "biny", "gain", "startexposure", "imageready", "imagearray", "cameraxsize")
injections = {}
unplugged = {"on": False, "message": "camera removed from USB", "error": 1031, "delay_ms": 0}
scope_unplugged = {"on": False, "error": 1280, "delay_ms": 0, "connect_delay_ms": 0,
                   "message": "The Sky-Watcher mount on /dev/ttyUSB0 did not reply (Timeout waiting for motor "
                              "controller reply to ':a1'). Check the Serial Port setting and that the mount is on."}
stall_ms = {"camera": 0, "telescope": 0}


def take_injected_error(call):
    """The (error, message, format, http) a camera call must answer with, or None. Guarded by state_lock."""
    inj = injections.get(call)
    if inj and inj["count"] > 0:
        inj["count"] -= 1
        if inj["count"] == 0:
            del injections[call]
        return inj["error"], inj["message"], inj["format"], inj["http"]
    if unplugged["on"] and call in INJECTABLE_CALLS:
        return unplugged["error"], unplugged["message"], "json", 200
    return None


def imagebytes_error(error, message):
    """An ASCOM ImageBytes response carrying an error: 44-byte metadata, then the UTF-8 message."""
    data_start = 44
    # MetadataVersion, ErrorNumber, ClientTransactionID, ServerTransactionID, DataStart,
    # ImageElementType, TransmissionElementType, Rank, Dimension1, Dimension2, Dimension3
    meta = struct.pack("<11I", 1, error, 0, 0, data_start, 0, 0, 0, 0, 0, 0)
    return meta + message.encode()


def ra_delta(ra_from, ra_to):
    """Shortest signed RA change in hours."""
    return (ra_to - ra_from + 12.0) % 24.0 - 12.0


def ra_at(t):
    """The scope's RA at monotonic time t (interpolated during a slew). Guarded by state_lock."""
    if t >= slew["t1"] or slew["t1"] <= slew["t0"]:
        return scope["ra"]
    f = max(0.0, (t - slew["t0"]) / (slew["t1"] - slew["t0"]))
    return (slew["ra0"] + ra_delta(slew["ra0"], slew["ra1"]) * f) % 24.0


def slewing_now():
    """Guarded by state_lock."""
    return time.monotonic() < slew["t1"]


def field_angle(t):
    """Field rotation (deg) at monotonic time t: the time-based rate, plus the
    scope's RA offset with --slew-rotates. Guarded by state_lock."""
    theta = ROTATE_DEG_PER_SEC * (t - T0)
    if SLEW_ROTATES:
        theta += -ra_delta(RA0, ra_at(t)) * 15.0
    return theta


def frame_angle_and_dim():
    """The (field angle, flux divisor) to draw the frame now being read with.
    Guarded by state_lock."""
    now = time.monotonic()
    if SLEW_SMEAR > 0.0 and exposure["dur"] > 0.0:
        e0, e1 = exposure["t0"], exposure["t0"] + exposure["dur"]
        o0, o1 = max(e0, slew["t0"]), min(e1, slew["t1"])
        if o1 > o0:
            return field_angle((o0 + o1) / 2.0), SLEW_SMEAR
    return field_angle(now), 1.0


def random_stars(n, seed):
    """n stars per frame area, spread over [-W/2, 1.5W] x [-H/2, 1.5H]."""
    import numpy as np

    rng = np.random.default_rng(seed)
    count = 4 * n
    xs = rng.uniform(-W / 2, 1.5 * W, count)
    ys = rng.uniform(-H / 2, 1.5 * H, count)
    fluxes = np.exp(rng.uniform(math.log(3000), math.log(40000), count))
    return [(float(x), float(y), float(f)) for x, y, f in zip(xs, ys, fluxes)]


def add_star(img, cx, cy, flux):
    """Add a sigma-2 Gaussian star; only the pixels within 12 px are touched."""
    import numpy as np

    x0, x1 = max(0, int(cx) - 12), min(W, int(cx) + 13)
    y0, y1 = max(0, int(cy) - 12), min(H, int(cy) + 13)
    if x0 >= x1 or y0 >= y1:
        return
    yy, xx = np.mgrid[y0:y1, x0:x1]
    img[y0:y1, x0:x1] += flux * np.exp(-(((xx - cx) ** 2 + (yy - cy) ** 2) / (2 * 2.0 ** 2)))


def render(theta_deg, dim=1.0):
    """The frame as a [y][x] numpy array of ints."""
    import numpy as np

    rng = np.random.default_rng(7)
    img = rng.normal(800, 15, (H, W))
    th = math.radians(theta_deg)
    px, py = POLE
    # pulses + injected dec drift + bolt-turn shifts move the whole field
    ox = scope["pulse_x"] + shift["x"]
    oy = scope["pulse_y"] + shift["y"] + DEC_DRIFT_PX_PER_MIN * (time.monotonic() - T0) / 60.0
    for sx, sy, flux in STARS:
        rx, ry = sx - px, sy - py
        cx = px + rx * math.cos(th) - ry * math.sin(th) + ox
        cy = py + rx * math.sin(th) + ry * math.cos(th) + oy
        add_star(img, cx, cy, flux / dim)
    for dx, dy, flux in decoys:
        add_star(img, dx, dy, flux / dim)
    return img.clip(0, 65535).astype(int)


def star_field():
    """Synthetic frame ([y][x] array): noisy background + Gaussian stars, optionally
    rotated about the fake pole by the elapsed-time angle."""
    global _frame
    with state_lock:
        theta, dim = frame_angle_and_dim()
        if (theta != 0.0 or dim != 1.0 or DEC_DRIFT_PX_PER_MIN != 0.0 or scope["pulse_x"] or scope["pulse_y"]
                or shift["x"] or shift["y"] or decoys):
            return render(theta, dim)
        if _frame is None:
            _frame = render(0.0)
        return _frame


def imagebytes_frame(img):
    """An ASCOM ImageBytes response: 44-byte metadata, then UInt16 pixels, x-major."""
    data_start = 44
    # MetadataVersion, ErrorNumber, ClientTransactionID, ServerTransactionID, DataStart,
    # ImageElementType (Int32=2), TransmissionElementType (UInt16=8), Rank, Dimension1..3
    meta = struct.pack("<11I", 1, 0, 0, 0, data_start, 2, 8, 2, W, H, 0)
    return meta + img.T.astype("<u2").tobytes()


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
    "gain": 0,
    "gainmin": 0,
    "gainmax": 100,
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
        pulses.append({"direction": direction, "duration": duration_ms})
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
        self.send_body(body, "application/json")

    def send_body(self, body, content_type):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def reply_error(self, error, message, fmt="json", http=200):
        """An Alpaca error, HTTP 200 like a real server; or, with http != 200, that HTTP status."""
        if http != 200:
            self.log_message("injected HTTP %d on %s: %s", http, self.path, message)
            body = message.encode()
            self.send_response(http)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return None
        self.log_message("injected error %d on %s: %s", error, self.path, message)
        if fmt == "imagebytes":
            return self.send_body(imagebytes_error(error, message), "application/imagebytes")
        body = json.dumps({"ErrorNumber": error, "ErrorMessage": message}).encode()
        return self.send_body(body, "application/json")

    def injected(self, call):
        """Answer with an injected error if one is due; True when it did."""
        with state_lock:
            err = take_injected_error(call)
        if err is None:
            return False
        self.reply_error(*err)
        return True

    def control(self, method, body):
        """The /fake/* error-injection control endpoint."""
        name, _, query = self.path.partition("?")
        params = {k: v[-1] for k, v in parse_qs(query if method == "GET" else body, keep_blank_values=True).items()}
        with state_lock:
            if method == "GET" and name == "/fake/pulses":
                body = json.dumps({"pulses": list(pulses)}).encode()
                if params.get("clear", "0").lower() in ("1", "true"):
                    pulses.clear()
                return self.send_body(body, "application/json")
            if method == "PUT" and name == "/fake/inject":
                call = params.get("call", "")
                if call not in INJECTABLE_CALLS:
                    return self.send_body(b'{"error":"unknown call"}', "application/json")
                count = int(params.get("count", "1"))
                if count <= 0:
                    injections.pop(call, None)
                else:
                    injections[call] = {"error": int(params.get("error", "1031")), "count": count,
                                        "message": params.get("message", "injected error"),
                                        "format": params.get("format", "json"),
                                        "http": int(params.get("http", "200"))}
            elif method == "PUT" and name == "/fake/shift":
                dx, dy = float(params.get("dx", "0")), float(params.get("dy", "0"))
                if params.get("absolute", "false").lower() == "true":
                    shift.update(x=dx, y=dy)
                else:
                    shift.update(x=shift["x"] + dx, y=shift["y"] + dy)
            elif method == "PUT" and name == "/fake/decoy":
                if params.get("clear", "false").lower() == "true":
                    decoys.clear()
                else:
                    decoys.append((float(params["x"]), float(params["y"]), float(params.get("flux", "30000"))))
            elif method == "PUT" and name == "/fake/unplug":
                device = params.get("device", "camera")
                if device not in ("camera", "telescope"):
                    return self.send_body(b'{"error":"unknown device"}', "application/json")
                plug = unplugged if device == "camera" else scope_unplugged
                plug["on"] = params.get("unplugged", "true").lower() == "true"
                if "message" in params:
                    plug["message"] = params["message"]
                if "error" in params:
                    plug["error"] = int(params["error"])
                if "delay_ms" in params:
                    plug["delay_ms"] = int(params["delay_ms"])
                if "connect_delay_ms" in params and device == "telescope":
                    plug["connect_delay_ms"] = max(0, int(params["connect_delay_ms"]))
                if plug["on"]:
                    if device == "camera":
                        state["connected"] = False
                    else:
                        scope["connected"] = False
            elif method == "PUT" and name == "/fake/stall":
                device = params.get("device", "camera")
                if device not in stall_ms:
                    return self.send_body(b'{"error":"unknown device"}', "application/json")
                stall_ms[device] = max(0, int(params.get("ms", "0")))
            body = json.dumps({"injections": injections, "unplugged": unplugged,
                               "telescope_unplugged": scope_unplugged, "stall": stall_ms,
                               "shift": shift, "decoys": decoys}).encode()
        self.log_message("control %s %s %s", method, name, params)
        return self.send_body(body, "application/json")

    def prop_name(self):
        m = re.match(r"^/api/v1/camera/0/(\w+)", self.path)
        return m.group(1) if m else None

    def scope_prop_name(self):
        m = re.match(r"^/api/v1/telescope/0/(\w+)", self.path)
        return m.group(1) if m else None

    def stall(self, device):
        """Sleep the /fake/stall time for the device, outside the lock."""
        with state_lock:
            ms = stall_ms[device]
        if ms > 0:
            self.log_message("stalling %s %d ms: %s", device, ms, self.path)
            time.sleep(ms / 1000.0)

    def do_GET(self):
        if self.path.startswith("/fake/"):
            return self.control("GET", "")
        sprop = self.scope_prop_name()
        if sprop is not None:
            self.stall("telescope")
            if sprop == "connected":
                with state_lock:
                    return self.reply(scope["connected"])
            if sprop == "rightascension":
                with state_lock:
                    return self.reply(ra_at(time.monotonic()))
            if sprop == "slewing":
                with state_lock:
                    return self.reply(slewing_now())
            if sprop == "declination":
                return self.reply(scope["dec"])
            if sprop == "siderealtime":
                return self.reply(lst_hours())
            if sprop in SCOPE_PROPS:
                return self.reply(SCOPE_PROPS[sprop])
            self.log_message("unknown scope GET %s", self.path)
            return self.reply(0)

        prop = self.prop_name()
        if prop is not None:
            self.stall("camera")
        if prop == "connected":
            with state_lock:
                value = state["connected"]
            return self.reply(value)
        if prop == "imageready":
            if self.injected("imageready"):
                return None
            with state_lock:
                value = state["imageready"]
                if value and READOUT_MS > 0:
                    exposure["polls"] += 1
                    past_ms = (time.monotonic() - exposure["t0"] - exposure["dur"]) * 1000.0
                    value = past_ms >= READOUT_MS
                    if value and not exposure["logged"]:
                        exposure["logged"] = True
                        self.log_message("imageready true after %d polls (%.0f ms past the exposure end, readout %d ms)",
                                         exposure["polls"], past_ms, READOUT_MS)
            return self.reply(value)
        if prop == "imagearray":
            if self.injected("imagearray"):
                return None
            img = star_field()
            if IMAGEBYTES and "application/imagebytes" in (self.headers.get("Accept") or ""):
                return self.send_body(imagebytes_frame(img), "application/imagebytes")
            # Alpaca imagearray is [x][y] (column-major)
            return self.reply(img.T.tolist())
        if prop in INJECTABLE_CALLS and self.injected(prop):
            return None
        if prop in PROPS:
            return self.reply(PROPS[prop])
        self.log_message("unknown GET %s", self.path)
        return self.reply(0)

    def do_PUT(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length).decode() if length else ""

        if self.path.startswith("/fake/"):
            return self.control("PUT", body)

        sprop = self.scope_prop_name()
        if sprop is not None:
            self.stall("telescope")
            params = dict(p.split("=", 1) for p in body.split("&") if "=" in p)
            if sprop == "connected":
                with state_lock:
                    want = "true" in body.lower()
                    refused = want and scope_unplugged["on"]
                    slow = want and not refused and not scope["connected"] and scope_unplugged["connect_delay_ms"] > 0
                    if not refused and not slow:
                        scope["connected"] = want
                    error, message, delay = scope_unplugged["error"], scope_unplugged["message"], scope_unplugged["delay_ms"]
                    connect_delay = scope_unplugged["connect_delay_ms"]
                if slow:
                    time.sleep(connect_delay / 1000.0)
                    with state_lock:
                        scope["connected"] = True
                if refused:
                    # AlpacaBridge holds the request while the driver times out on the serial port
                    time.sleep(delay / 1000.0)
                    return self.reply_error(error, message)
            elif sprop == "pulseguide":
                apply_pulse(int(params.get("Direction", -1)), float(params.get("Duration", 0)))
            elif sprop in ("slewtocoordinates", "slewtocoordinatesasync"):
                with state_lock:
                    now = time.monotonic()
                    ra_from = ra_at(now)
                    scope["ra"] = float(params.get("RightAscension", scope["ra"]))
                    scope["dec"] = float(params.get("Declination", scope["dec"]))
                    if SLEW_SECONDS > 0.0:
                        slew.update(t0=now, t1=now + SLEW_SECONDS, ra0=ra_from, ra1=scope["ra"])
                        self.log_message("slew RA %.4f -> %.4f over %.1f s", ra_from, scope["ra"], SLEW_SECONDS)
            elif sprop == "abortslew":
                with state_lock:
                    now = time.monotonic()
                    if now < slew["t1"]:
                        scope["ra"] = ra_at(now)
                        slew["t1"] = now
                        self.log_message("slew aborted at RA %.4f", scope["ra"])
            else:
                self.log_message("unknown scope PUT %s body=%s", self.path, body)
            return self.reply(None)

        prop = self.prop_name()
        if prop is not None:
            self.stall("camera")
        if prop in INJECTABLE_CALLS and self.injected(prop):
            return None
        if prop == "connected":
            with state_lock:
                want = "true" in body.lower()
                refused = want and unplugged["on"]
                if not refused:
                    state["connected"] = want
                error, message, delay = unplugged["error"], unplugged["message"], unplugged["delay_ms"]
            if refused:
                time.sleep(delay / 1000.0)
                return self.reply_error(error, message)
        elif prop == "startexposure":
            params = dict(p.split("=", 1) for p in body.split("&") if "=" in p)
            with state_lock:
                state["imageready"] = True
                exposure.update(t0=time.monotonic(), dur=float(params.get("Duration", 0) or 0), polls=0, logged=False)
        elif prop in ("abortexposure", "stopexposure"):
            with state_lock:
                state["imageready"] = False
        elif prop == "gain":
            m = re.search(r"(?:^|&)Gain=(-?\d+)", body)
            if m:
                PROPS["gain"] = int(m.group(1))
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
    ap.add_argument("--slew-seconds", type=float, default=0.0,
                    help="slewtocoordinatesasync takes this long and reads slewing meanwhile (0: instant)")
    ap.add_argument("--slew-rotates", action="store_true",
                    help="rotate the star field about --pole by the scope's RA change (RA slews)")
    ap.add_argument("--slew-smear", type=float, default=0.0, metavar="F",
                    help="divide star flux by F on frames exposed during a slew (0: off)")
    ap.add_argument("--size", default="640,480", help="sensor W,H in px")
    ap.add_argument("--stars", type=int, default=0, metavar="N",
                    help="N random stars per frame area instead of the dozen fixed ones (0: fixed)")
    ap.add_argument("--star-seed", type=int, default=1, help="random field for --stars")
    ap.add_argument("--imagebytes", action="store_true",
                    help="serve imagearray as ASCOM ImageBytes when the client accepts it")
    ap.add_argument("--readout-ms", type=float, default=0.0, metavar="N",
                    help="imageready reads false until N ms after the exposure is due (0: true at startexposure)")
    args = ap.parse_args()
    W, H = (int(v) for v in args.size.split(","))
    PROPS.update(cameraxsize=W, cameraysize=H, numx=W, numy=H)
    if args.stars > 0:
        STARS = random_stars(args.stars, args.star_seed)
    IMAGEBYTES = args.imagebytes
    READOUT_MS = args.readout_ms
    SLEW_SECONDS = args.slew_seconds
    SLEW_ROTATES = args.slew_rotates
    SLEW_SMEAR = args.slew_smear
    ROTATE_DEG_PER_SEC = args.rotate_deg_per_sec
    POLE = tuple(float(v) for v in args.pole.split(","))
    DEC_DRIFT_PX_PER_MIN = args.dec_drift_px_per_min
    IMAGE_SCALE = args.image_scale
    print(f"fake-alpaca: serving camera+telescope 0 on {args.host}:{args.port}"
          f" (rotation {ROTATE_DEG_PER_SEC} deg/s about {POLE}, dec drift {DEC_DRIFT_PX_PER_MIN} px/min,"
          f" slew {SLEW_SECONDS} s rotates={SLEW_ROTATES} smear={SLEW_SMEAR}, {W}x{H}, {len(STARS)} stars"
          f"{', imagebytes' if IMAGEBYTES else ''}{f', readout {READOUT_MS:g} ms' if READOUT_MS > 0 else ''})",
          flush=True)
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()
