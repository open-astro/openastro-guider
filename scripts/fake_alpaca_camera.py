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
misalignment - what the Guiding Assistant's polar-alignment estimate measures.
Slew endpoints update the reported coordinates only, unless --slew-seconds is
given:

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
    PUT /fake/hold     frames=<N>
        The next N exposures never complete: imageready reads false from their
        startexposure until the next startexposure (a camera that stopped reading
        out, for the guider's capture timeout, #142); frames=0 clears it. The
        next exposure after them is a normal one.
    GET /fake/state    the injections, the unplugged flags, the stalls and the holds, as JSON.

Alpaca management (#155, for the guider's query_alpaca_devices):

    GET /management/v1/configureddevices
        The camera and the telescope at device 0, as an Alpaca server lists them.

Setup checks (#117: set_profile_setup must reach the live mount and camera objects):

    GET /fake/pulses[?clear=1]
        The telescope pulseguide requests since the last clear (the latest 1000), as
        {"pulses": [{"direction": 0-3 (N S E W), "duration": ms}, ...]}; clear=1
        returns them and empties the list.

    e.g. curl -X PUT -d 'call=imageready&error=1280&count=3&message=acq failed' \
             http://127.0.0.1:11111/fake/inject

Telescope faults (#142): PUT /fake/inject with device=telescope and call=<pulseguide|declination|
rightascension|siderealtime|sideofpier|slewing|ispulseguiding|guideratedeclination|guideraterightascension>
answers the next N requests for that telescope property with the Alpaca error (the error, count, message
and http parameters as for the camera). An injected pulseguide error does not move the mount.

Mount and sky physics (#142, the daemon e2e calibration, dither and clouds scenarios). Every option is off
by default, and with none given the frames, pulses and readings are exactly what they were before:

    The mount keeps the guide pulses as RA and Dec offsets in pixels (mount axes, West and North
    positive) and the camera maps them onto the sensor. In the guider's convention (calibration_run.cpp,
    what get_calibration_data reports), --camera-angle A is the true xAngle, the direction an East pulse
    moves the star (a West pulse moves it towards A + 180 deg), and yAngle, the direction a North pulse
    moves it, is A - 90 deg (A + 90 deg with --dec-mirror). A=0 without the mirror is the mapping the
    fake always had (East +x, North -y).

    --camera-angle DEG  the true xAngle, deg (default 0)
    --dec-mirror        yAngle = xAngle + 90 deg instead of - 90 deg (a mirrored image)
    --cos-dec           a RA pulse moves the field rate x duration x cos(Dec), with the Dec the telescope
                        reads at that moment (the Dec leg is not scaled); without it the RA rate is the
                        Dec 0 rate at every Dec, as before
    --dec-backlash-ms MS
                        Dec backlash as dead time: the first MS ms of Dec pulse after a reversal of the
                        Dec direction do not move the field (the mount starts engaged South, so the first
                        North pulses take it up)
    --sim-drift RA,DEC  a steady drift in mount axes, px per simulated minute
    --pe-px AMP, --pe-period-s P
                        a sinusoidal RA periodic error of AMP px and period P simulated seconds
                        (default 480)
    --seeing-px SIGMA   per-frame Gaussian jitter of the whole field, px per sensor axis
    --frame-noise       a new Gaussian background each frame instead of the same one every frame
    --pier-flip         sideofpier follows the slews: a slew to a target west of the meridian (hour angle
                        >= 0, from siderealtime) reads pierEast (0), east of it pierWest (1); a slew that
                        changes it is a meridian flip, which turns the RA axis 180 deg on the sensor and
                        leaves the Dec motion where it was, so the image is mirrored as a German
                        equatorial mount's is to a guider that does not flip Dec (CalFlipRequiresDecFlip
                        false, the default). The field stays where it was at the flip. Without it
                        sideofpier reads 0, as before.
    --seed N            seeds the seeing and the frame noise once, at start (default 1); the same
                        requests give the same frames

    Simulated time advances by each startexposure's Duration and by each guide pulse's duration, so
    the drift and the periodic error do not depend on how fast the host runs (a sanitizer build on a
    slow runner sees the same sky). The frame shows the field at the end of its exposure.

    PUT /fake/clouds   frames=<N>[&dim=<F, default 1e6>]
        The next N frames read have every field star's flux divided by F; frames=0 clears it.
        Decoys are dimmed too. The default is a thick cloud: the brightest star (40000 ADU) peaks
        at 0.04 ADU against the 15 ADU noise, so no star is found, only noise (the guider reads
        low SNR or low mass). A thin cloud is findable: at F=1000 that star peaks at 40 ADU and the
        guider still finds it on about a third of the frames at SNR 9-12, which only its
        mass-change check rejects.
    GET /fake/truth
        What the guider should measure, as JSON: the true xAngle and yAngle (deg, -180..180, the pier
        flip included), ra_rate_px_per_ms and dec_rate_px_per_ms (the RA rate at the telescope's current
        Dec with --cos-dec), the Dec, the side of pier, the backlash (ms, and what is left of it), the
        drift, the periodic error, the seeing, the simulated time, the mount's RA/Dec offset and the
        field's offset on the sensor (px, without the seeing jitter) and the clouds left.

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
# ra_px/dec_px: the pulses (and the simulated drift and PE) in mount axes, West and North positive
scope = {"connected": False, "ra": 12.0, "dec": 30.0, "ra_px": 0.0, "dec_px": 0.0}

# Mount and sky physics (#142; all off by default, see the module docstring)
CAMERA_ANGLE_DEG = 0.0
DEC_MIRROR = False
COS_DEC = False
DEC_BACKLASH_MS = 0.0
SIM_DRIFT_PX_PER_MIN = (0.0, 0.0)  # RA, Dec
PE_PX = 0.0
PE_PERIOD_S = 480.0
SEEING_PX = 0.0
FRAME_NOISE = False
PIER_FLIP = False
SEED = 1
rngs = {}  # "seeing", "noise": numpy Generators made once from SEED at start
# simulated seconds: each exposure's Duration plus each guide pulse's duration
sim = {"t": 0.0}
# the Dec direction the gears are engaged in, and the backlash still to take up after a reversal
backlash = {"north": False, "slack_ms": 0.0}
# side: Alpaca SideOfPier (0 pierEast, 1 pierWest); flipped: the RA axis is turned 180 deg on the sensor;
# base_x/base_y: the field offset folded in at a flip, so the field does not jump
pier = {"side": 0, "flipped": False, "base_x": 0.0, "base_y": 0.0}
clouds = {"frames": 0, "dim": 1e6}
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
# PUT /fake/hold: the exposures still to hold (never complete), and whether the one in progress is held
hold = {"frames": 0, "held": False}

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
# telescope faults: PUT /fake/inject with device=telescope
SCOPE_INJECTABLE_CALLS = ("pulseguide", "declination", "rightascension", "siderealtime", "sideofpier", "slewing",
                          "ispulseguiding", "guideratedeclination", "guideraterightascension")
scope_injections = {}
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


def take_scope_injected_error(call):
    """The (error, message, format, http) a telescope call must answer with, or None. Guarded by state_lock."""
    inj = scope_injections.get(call)
    if inj and inj["count"] > 0:
        inj["count"] -= 1
        if inj["count"] == 0:
            del scope_injections[call]
        return inj["error"], inj["message"], "json", inj["http"]
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


def cos_sin_deg(deg):
    """(cos, sin) of an angle in degrees, exact for multiples of 90 so the default mapping stays exact."""
    quarter = deg / 90.0
    if quarter == int(quarter):
        return ((1.0, 0.0), (0.0, 1.0), (-1.0, 0.0), (0.0, -1.0))[int(quarter) % 4]
    rad = math.radians(deg)
    return math.cos(rad), math.sin(rad)


def norm_deg(deg):
    """An angle in (-180, 180]."""
    deg = math.fmod(deg, 360.0)
    if deg > 180.0:
        deg -= 360.0
    elif deg <= -180.0:
        deg += 360.0
    return deg


def true_angles():
    """The guider-convention (xAngle, yAngle), deg: an East pulse moves the star towards xAngle (West
    towards xAngle + 180), a North pulse towards yAngle. A meridian flip turns the RA axis only.
    Guarded by state_lock."""
    x_angle = CAMERA_ANGLE_DEG + (180.0 if pier["flipped"] else 0.0)
    y_angle = CAMERA_ANGLE_DEG + (90.0 if DEC_MIRROR else -90.0)
    return x_angle, y_angle


def field_offset():
    """The field's translation on the sensor (px, x and y) now, without the seeing jitter: the mount's
    RA/Dec offsets mapped by the camera, the offset folded in at a flip, the bolt-turn shifts and the
    legacy wall-clock --dec-drift-px-per-min. Guarded by state_lock."""
    mx, my = mount_offset_on_sensor()
    return mx + shift["x"], my + shift["y"] + DEC_DRIFT_PX_PER_MIN * (time.monotonic() - T0) / 60.0


def mount_offset_on_sensor():
    """The mount's RA/Dec offsets mapped onto the sensor (px), plus what a flip folded in. Guarded by state_lock."""
    x_angle, y_angle = true_angles()
    xc, xs = cos_sin_deg(x_angle)
    yc, ys = cos_sin_deg(y_angle)
    ra, dec = scope["ra_px"], scope["dec_px"]
    # West moves the star by -(cos, sin) of xAngle, North by +(cos, sin) of yAngle
    return -ra * xc + dec * yc + pier["base_x"], -ra * xs + dec * ys + pier["base_y"]


def advance_sim(dt):
    """Advance simulated time by dt seconds: the --sim-drift and --pe-px motion over it goes into the
    mount's offsets. Guarded by state_lock."""
    if dt <= 0.0:
        return
    t0 = sim["t"]
    sim["t"] = t0 + dt
    ra_drift, dec_drift = SIM_DRIFT_PX_PER_MIN
    if ra_drift:
        scope["ra_px"] += ra_drift * dt / 60.0
    if dec_drift:
        scope["dec_px"] += dec_drift * dt / 60.0
    if PE_PX and PE_PERIOD_S > 0.0:
        w = 2.0 * math.pi / PE_PERIOD_S
        scope["ra_px"] += PE_PX * (math.sin(w * sim["t"]) - math.sin(w * t0))


_background = None  # the same-every-frame background, made once (it used to be made again each frame)


def background():
    """The frame's background: a new Gaussian one each frame from the seeded generator with
    --frame-noise, otherwise the fixed one every frame (default_rng(7), as before)."""
    global _background
    import numpy as np

    if FRAME_NOISE:
        return rngs["noise"].standard_normal((H, W), dtype=np.float32) * np.float32(15.0) + np.float32(800.0)
    if _background is None:
        _background = np.random.default_rng(7).normal(800, 15, (H, W))
    return _background.copy()


def render(theta_deg, dim=1.0, jitter=(0.0, 0.0)):
    """The frame as a [y][x] numpy array of ints."""
    img = background()
    th = math.radians(theta_deg)
    px, py = POLE
    # pulses + drift + PE + bolt-turn shifts (+ seeing) move the whole field
    ox, oy = field_offset()
    ox += jitter[0]
    oy += jitter[1]
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
        if clouds["frames"] > 0:
            clouds["frames"] -= 1
            dim *= clouds["dim"]
        jitter = (0.0, 0.0)
        if SEEING_PX > 0.0:
            jx, jy = rngs["seeing"].normal(0.0, SEEING_PX, 2)
            jitter = (float(jx), float(jy))
        ox, oy = field_offset()
        if (theta != 0.0 or dim != 1.0 or ox or oy or decoys or FRAME_NOISE or SEEING_PX > 0.0
                or DEC_DRIFT_PX_PER_MIN != 0.0):
            return render(theta, dim, jitter)
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
    "guideratedeclination": GUIDE_RATE_DEG_S,
    "guideraterightascension": GUIDE_RATE_DEG_S,
    "sitelatitude": 45.0,
    "sitelongitude": -110.0,
}


def lst_hours():
    # crude advancing sidereal clock; absolute value is irrelevant for tests
    return (12.0 + (time.monotonic() - T0) * 1.0027 / 3600.0) % 24.0


def rates_px_per_ms():
    """The true (RA, Dec) guide rates on the sensor, px/ms, at the telescope's Dec now. Guarded by state_lock."""
    rate = GUIDE_RATE_DEG_S * 3600.0 / IMAGE_SCALE / 1000.0
    ra_rate = rate * math.cos(math.radians(scope["dec"])) if COS_DEC else rate
    return ra_rate, rate


def apply_pulse(direction, duration_ms):
    """Alpaca GuideDirections: 0=N 1=S 2=E 3=W. Move the mount by rate x duration in its axes: RA
    times cos(Dec) with --cos-dec, Dec less the backlash a reversal has to take up."""
    with state_lock:
        pulses.append({"direction": direction, "duration": duration_ms})
        moving_ms = duration_ms
        if direction in (0, 1) and DEC_BACKLASH_MS > 0.0:
            north = direction == 0
            if north != backlash["north"]:
                backlash.update(north=north, slack_ms=DEC_BACKLASH_MS)
            taken = min(float(duration_ms), backlash["slack_ms"])
            backlash["slack_ms"] -= taken
            moving_ms = duration_ms - taken
        px = GUIDE_RATE_DEG_S * (moving_ms / 1000.0) * 3600.0 / IMAGE_SCALE
        if direction in (2, 3) and COS_DEC:
            px *= math.cos(math.radians(scope["dec"]))
        if direction == 0:
            scope["dec_px"] += px
        elif direction == 1:
            scope["dec_px"] -= px
        elif direction == 2:
            scope["ra_px"] -= px
        elif direction == 3:
            scope["ra_px"] += px
        advance_sim(duration_ms / 1000.0)


def truth():
    """GET /fake/truth: what the guider should measure now. Guarded by state_lock."""
    x_angle, y_angle = true_angles()
    ra_rate, dec_rate = rates_px_per_ms()
    ox, oy = field_offset()
    pe_now = PE_PX * math.sin(2.0 * math.pi * sim["t"] / PE_PERIOD_S) if PE_PX and PE_PERIOD_S > 0.0 else 0.0
    return {
        "camera_angle_deg": CAMERA_ANGLE_DEG, "dec_mirror": DEC_MIRROR,
        "x_angle_deg": norm_deg(x_angle), "y_angle_deg": norm_deg(y_angle),
        "side_of_pier": pier["side"], "pier_flipped": pier["flipped"],
        "dec_deg": scope["dec"], "cos_dec": COS_DEC,
        "ra_rate_px_per_ms": ra_rate, "dec_rate_px_per_ms": dec_rate,
        "dec_backlash_ms": DEC_BACKLASH_MS, "backlash_left_ms": backlash["slack_ms"],
        "dec_engaged": "North" if backlash["north"] else "South",
        "drift_px_per_min": {"ra": SIM_DRIFT_PX_PER_MIN[0], "dec": SIM_DRIFT_PX_PER_MIN[1]},
        "pe": {"amplitude_px": PE_PX, "period_s": PE_PERIOD_S, "now_px": pe_now},
        "seeing_px": SEEING_PX, "frame_noise": FRAME_NOISE, "seed": SEED, "sim_time_s": sim["t"],
        "mount_offset_px": {"ra": scope["ra_px"], "dec": scope["dec_px"]},
        "field_offset_px": {"x": ox, "y": oy},
        "clouds": dict(clouds),
    }


def pier_side_for(ra_hours):
    """The Alpaca SideOfPier a German equatorial mount takes for a target at ra_hours: pierEast (0)
    looking west of the meridian (hour angle >= 0), pierWest (1) east of it."""
    return 0 if ra_delta(ra_hours, lst_hours()) >= 0.0 else 1


def update_pier_side(ra_hours):
    """--pier-flip: a slew that changes the side of pier is a meridian flip. Guarded by state_lock."""
    side = pier_side_for(ra_hours)
    if side == pier["side"]:
        return
    # fold the mount's offset in, so the flip turns the axes without moving the field
    bx, by = mount_offset_on_sensor()
    pier.update(base_x=bx, base_y=by, side=side, flipped=not pier["flipped"])
    scope["ra_px"] = scope["dec_px"] = 0.0
    print(f"fake-alpaca: meridian flip, sideofpier {side}", flush=True)


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
            if method == "GET" and name == "/fake/truth":
                return self.send_body(json.dumps(truth()).encode(), "application/json")
            if method == "PUT" and name == "/fake/inject":
                call = params.get("call", "")
                telescope = params.get("device", "camera") == "telescope"
                table = scope_injections if telescope else injections
                if call not in (SCOPE_INJECTABLE_CALLS if telescope else INJECTABLE_CALLS):
                    return self.send_body(b'{"error":"unknown call"}', "application/json")
                count = int(params.get("count", "1"))
                if count <= 0:
                    table.pop(call, None)
                else:
                    table[call] = {"error": int(params.get("error", "1031")), "count": count,
                                   "message": params.get("message", "injected error"),
                                   "format": params.get("format", "json"),
                                   "http": int(params.get("http", "200"))}
            elif method == "PUT" and name == "/fake/clouds":
                clouds.update(frames=max(0, int(params.get("frames", "0"))),
                              dim=max(1.0, float(params.get("dim", "1e6"))))
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
            elif method == "PUT" and name == "/fake/hold":
                hold["frames"] = max(0, int(params.get("frames", "0")))
                if hold["frames"] == 0:
                    hold["held"] = False
            body = json.dumps({"injections": injections, "telescope_injections": scope_injections,
                               "unplugged": unplugged, "telescope_unplugged": scope_unplugged, "stall": stall_ms,
                               "shift": shift, "decoys": decoys, "clouds": clouds, "hold": hold}).encode()
        self.log_message("control %s %s %s", method, name, params)
        return self.send_body(body, "application/json")

    def scope_injected(self, call):
        """Answer with an injected telescope error if one is due; True when it did."""
        with state_lock:
            err = take_scope_injected_error(call)
        if err is None:
            return False
        self.reply_error(*err)
        return True

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
        if self.path.partition("?")[0] == "/management/v1/configureddevices":
            return self.reply([
                {"DeviceName": PROPS["name"], "DeviceType": "Camera", "DeviceNumber": 0, "UniqueID": "fake-camera-0"},
                {"DeviceName": SCOPE_PROPS["name"], "DeviceType": "Telescope", "DeviceNumber": 0,
                 "UniqueID": "fake-telescope-0"},
            ])
        sprop = self.scope_prop_name()
        if sprop is not None:
            self.stall("telescope")
            if sprop == "connected":
                with state_lock:
                    return self.reply(scope["connected"])
            if self.scope_injected(sprop):
                return None
            if sprop == "sideofpier":
                with state_lock:
                    return self.reply(pier["side"])
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
                value = state["imageready"] and not hold["held"]
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
                if self.scope_injected(sprop):
                    return None
                apply_pulse(int(params.get("Direction", -1)), float(params.get("Duration", 0)))
            elif sprop in ("slewtocoordinates", "slewtocoordinatesasync"):
                with state_lock:
                    now = time.monotonic()
                    ra_from = ra_at(now)
                    scope["ra"] = float(params.get("RightAscension", scope["ra"]))
                    scope["dec"] = float(params.get("Declination", scope["dec"]))
                    if PIER_FLIP:
                        update_pier_side(scope["ra"])
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
                advance_sim(exposure["dur"])
                hold["held"] = hold["frames"] > 0
                if hold["held"]:
                    hold["frames"] -= 1
                    self.log_message("holding this exposure: imageready stays false (%d more to hold)", hold["frames"])
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
    # mount and sky physics (#142), all off by default
    ap.add_argument("--camera-angle", type=float, default=0.0, metavar="DEG",
                    help="the true xAngle in the guider's convention (0: the mapping the fake always had)")
    ap.add_argument("--dec-mirror", action="store_true", help="yAngle = xAngle + 90 deg (a mirrored image)")
    ap.add_argument("--cos-dec", action="store_true",
                    help="RA pulses move the field cos(Dec) as far, at the telescope's current Dec")
    ap.add_argument("--dec-backlash-ms", type=float, default=0.0, metavar="MS",
                    help="Dec backlash: dead time after each reversal of the Dec direction (0: none)")
    ap.add_argument("--sim-drift", default="0,0", metavar="RA,DEC",
                    help="steady drift in mount axes, px per simulated minute")
    ap.add_argument("--pe-px", type=float, default=0.0, metavar="AMP",
                    help="sinusoidal RA periodic error amplitude, px")
    ap.add_argument("--pe-period-s", type=float, default=480.0, metavar="P",
                    help="periodic error period, simulated seconds")
    ap.add_argument("--seeing-px", type=float, default=0.0, metavar="SIGMA",
                    help="per-frame Gaussian jitter of the field, px per axis")
    ap.add_argument("--frame-noise", action="store_true", help="a new Gaussian background each frame")
    ap.add_argument("--pier-flip", action="store_true",
                    help="sideofpier follows the slews; a slew that changes it is a meridian flip")
    ap.add_argument("--seed", type=int, default=1, help="seeds --seeing-px and --frame-noise once, at start")
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
    CAMERA_ANGLE_DEG = args.camera_angle
    DEC_MIRROR = args.dec_mirror
    COS_DEC = args.cos_dec
    DEC_BACKLASH_MS = max(0.0, args.dec_backlash_ms)
    SIM_DRIFT_PX_PER_MIN = tuple(float(v) for v in args.sim_drift.split(","))
    PE_PX = args.pe_px
    PE_PERIOD_S = args.pe_period_s
    SEEING_PX = max(0.0, args.seeing_px)
    FRAME_NOISE = args.frame_noise
    PIER_FLIP = args.pier_flip
    SEED = args.seed
    if SEEING_PX > 0.0 or FRAME_NOISE:
        import numpy as np

        # one generator each, made once, so turning one on does not change the other's sequence
        rngs.update(seeing=np.random.default_rng([SEED, 1]), noise=np.random.default_rng([SEED, 2]))
    physics = [f"camera angle {CAMERA_ANGLE_DEG:g}" if CAMERA_ANGLE_DEG else "", "dec mirror" if DEC_MIRROR else "",
               "cos(dec)" if COS_DEC else "", f"dec backlash {DEC_BACKLASH_MS:g} ms" if DEC_BACKLASH_MS else "",
               f"sim drift {args.sim_drift} px/min" if any(SIM_DRIFT_PX_PER_MIN) else "",
               f"PE {PE_PX:g} px / {PE_PERIOD_S:g} s" if PE_PX else "",
               f"seeing {SEEING_PX:g} px" if SEEING_PX else "", "frame noise" if FRAME_NOISE else "",
               "pier flip" if PIER_FLIP else ""]
    physics = [p for p in physics if p]
    print(f"fake-alpaca: serving camera+telescope 0 on {args.host}:{args.port}"
          f" (rotation {ROTATE_DEG_PER_SEC} deg/s about {POLE}, dec drift {DEC_DRIFT_PX_PER_MIN} px/min,"
          f" slew {SLEW_SECONDS} s rotates={SLEW_ROTATES} smear={SLEW_SMEAR}, {W}x{H}, {len(STARS)} stars"
          f"{', imagebytes' if IMAGEBYTES else ''}{f', readout {READOUT_MS:g} ms' if READOUT_MS > 0 else ''}"
          f"{'; physics: ' + ', '.join(physics) + f', seed {SEED}' if physics else ''})",
          flush=True)
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()
