#!/usr/bin/env python3
"""Daemon end-to-end run against the fake Alpaca server (#142 Tier 1).

Starts scripts/fake_alpaca_camera.py and the guider daemon (phd2.bin, headless under xvfb-run with a scratch HOME),
then drives the daemon over JSON-RPC the way Ara does. Four lanes run at once, each with its own fake, daemon,
instance, port and display, so the others should cost little wall time beside the main lane:

the main lane, on the plain fake (--instance, --fake-port, --display, --work-dir):

  http-before    before the first frame, the HTTP data port (8080 + instance - 1): /api/frame.jpg 404 "no image
                 available", other paths, a POST and ../-style /api/capture names 404 "Not found" (N2)
  smoke          scripts/phd2_rpc_smoke.py with --fake-alpaca-url
  nr-at-schedule once connected, capture_single_frame with no noise reduction and set_noise_reduction
                 "3x3 Median" during the exposure: the saved frame is unfiltered, and the next one filtered (#210)
  http-frame     after a single frame, /api/frame.jpg: 200 image/jpeg with Content-Length and an ETag "f<N>", 304 on
                 a matching If-None-Match, a baseline JPEG of the frame's size with 3 components whose grey level
                 matches the frame's FITS through the display stretch, the fake's stars where it put them; the ETag
                 "f1" again on the next single frame (pinned), rising while looping; /api/capture serving the file
                 save_image wrote, and 404 for ../-style names, a POST and a symlink out of the capture dir (N2)
  calibrate      connect the Alpaca camera and mount, loop, find_star, guide with recalibrate true: a full
                 calibration, StartGuiding, a settle that ends in exactly one SettleDone with Status 0, then
                 guiding; the event order Ara consumes is checked on the way (design/API_CONTRACT.md)
  new-target     stop, slew the fake mount to another target, guide with recalibrate true: a second full
                 calibration, made at the new Dec
  history        while guiding, get_guide_history's points match the GuideStep events: each "t" an integer
                 millisecond UTC time within 0.2 s of its GuideStep's Timestamp, and "ra_dir" / "dec_dir" the
                 first letter of RADirection / DECDirection ("E", "W", "N", "S"), or "" with no pulse (#133)
  frame-drop     while guiding, the camera fails 1 frame, then 2 in a row, with Alpaca 1280: each one is a
                 StarLost and guiding goes on (#136)

the physics lane, on a fake with a 37 deg camera, RA moving cos(Dec) as far, 800 ms of Dec backlash, a drift and
RA periodic error in simulated time, seeing and per-frame noise (PHYSICS_FAKE_ARGS; instance, fake port and display
+ 1, <work-dir>/physics), one calibration for all three:

  truth          slew to Dec 60, guide with recalibrate true: get_calibration_data's xAngle/yAngle within 2 deg of
                 the fake's /fake/truth and xRate/yRate within 5 %
  dither         dither 3 px with a settle: the event order, exactly one SettleDone with Status 0, the lock moving
                 by the dither mapped through the true camera angles (as a vector), the GuideSteps starting the
                 lock's move away from the new lock and corrected to within the settle pixels, and the true star
                 following the lock
  clouds         /fake/clouds hides the stars for 5 frames: 5 StarLost, each "Star lost - low SNR" or "Star lost -
                 low mass" (no star found), LostLock or Guiding meanwhile, guiding not stopped, and GuideSteps
                 again from the first clear frame

and the gear and faults lanes of scripts/e2e_equipment_scenarios.py (instance, fake port and display + 2 and + 3,
<work-dir>/gear and <work-dir>/faults; see its docstring): connect side effects, the #139 latch, auto-connect and the
#104 retry; camera and mount faults, the #105 reconnect, the Guiding Assistant and its restore.

Then each lane shuts its daemon down over RPC. A lane fails on a failed check, on the daemon exiting before the
shutdown or with a non-zero status, and on any AddressSanitizer / UndefinedBehaviorSanitizer report in the daemon's
output; the run fails when any lane does. Everything it collects (the daemon's stdout/stderr and debug log, the
fake's log, the smoke output, every event received) is left in --work-dir (each child lane's in its own subdir).

    python3 scripts/e2e_guide_scenarios.py --phd2 build-asan/phd2.bin --work-dir /tmp/e2e
    python3 scripts/e2e_guide_scenarios.py --lane physics ...   # one lane only
    python3 scripts/e2e_guide_scenarios.py --child-lanes gear ...   # the main lane and the gear lane
"""
import argparse
import json
import math
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import traceback
import urllib.parse
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The sanitizer settings the run uses unless the caller's environment already sets them. halt_on_error makes the
# first report fatal, so it cannot scroll past unnoticed; print_stacktrace gives UBSan reports a stack.
# detect_leaks=0: LeakSanitizer runs at exit and reports the allocations wxWidgets/GTK and the inherited PHD2
# singletons never free (they live until the process ends, by design), so a leak check would fail every run on
# memory that is not a bug; the use-after-free, overflow and UB checks stay on.
SANITIZER_ENV = {
    "ASAN_OPTIONS": "halt_on_error=1:abort_on_error=1:detect_leaks=0:print_stacktrace=1",
    "UBSAN_OPTIONS": "halt_on_error=1:abort_on_error=1:print_stacktrace=1",
}
# Known findings in code this run does not change, each reported; always added to UBSAN_OPTIONS.
UBSAN_SUPPRESSIONS = os.path.join(REPO, "scripts", "e2e_ubsan.supp")
# What a sanitizer report starts with, in the daemon's output.
SANITIZER_REPORT = re.compile(r"ERROR: AddressSanitizer|runtime error:|SUMMARY: (Address|UndefinedBehavior)Sanitizer")

# The events of a guide request that design/API_CONTRACT.md documents the order of.
CONTRACT_EVENTS = (
    "StartCalibration", "Calibrating", "CalibrationComplete", "CalibrationFailed", "StartGuiding", "SettleBegin",
    "Settling", "SettleDone", "GuideStep", "GuidingStopped", "StarLost",
)


class Failure(Exception):
    pass


class RunTimeout(Exception):
    """The whole-run timeout. step() re-raises it, so the run ends and goes to teardown."""


def expect(cond, msg):
    if not cond:
        raise Failure(msg)


class Daemon:
    """The daemon under xvfb-run, with its output in a file."""

    def __init__(self, args, work, log_name="phd2.out", wrap=(), extra_args=()):
        """`log_name` is the output file in `work`; `wrap` goes in front of the daemon's command line (strace), and
        `extra_args` after it (`-l <file>`, `-R`)."""
        self.home = os.path.join(work, "home")
        os.makedirs(self.home, exist_ok=True)
        self.log_path = os.path.join(work, log_name)
        env = dict(os.environ, HOME=self.home)
        for k, v in SANITIZER_ENV.items():
            env.setdefault(k, v)
        env["UBSAN_OPTIONS"] += f":suppressions={UBSAN_SUPPRESSIONS}"
        cmd = ["xvfb-run"]
        # -n before -a: -a then looks for a free display from that number on (e2e_persistence.py's args have no
        # --auto-display-from: 99 is this script's default)
        auto_from = getattr(args, "auto_display_from", 99)
        cmd += ["-n", str(auto_from), "-a"] if args.display == "auto" else ["-n", args.display]
        cmd += ["-s", "-screen 0 1280x1024x24", *wrap, os.path.abspath(args.phd2), "-i", str(args.instance),
                *extra_args]
        self.log = open(self.log_path, "wb")
        print(f"e2e: starting {' '.join(cmd)} (HOME={self.home})", flush=True)
        self.proc = subprocess.Popen(cmd, env=env, stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True)

    def exited(self):
        return self.proc.poll() is not None

    def check_alive(self):
        if self.exited():
            raise Failure(f"the daemon exited (status {self.proc.returncode}) while the run was going on")

    def wait(self, timeout):
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def kill(self):
        """Stop xvfb-run, the daemon and the Xvfb it started (one process group)."""
        if self.exited():
            return
        for sig, wait_s in ((signal.SIGTERM, 15), (signal.SIGKILL, 5)):
            try:
                os.killpg(self.proc.pid, sig)
            except ProcessLookupError:
                return
            if self.wait(wait_s) is not None:
                return

    def sanitizer_reports(self):
        self.log.flush()
        with open(self.log_path, "rb") as f:
            text = f.read().decode("utf-8", errors="replace")
        return [line for line in text.splitlines() if SANITIZER_REPORT.search(line)]


class Fake:
    """scripts/fake_alpaca_camera.py and its control endpoints."""

    def __init__(self, port, work, extra_args=(), log_name="fake_alpaca.log"):
        """`log_name` is the output file in `work`: a fake started again on the same port takes another one."""
        self.host, self.port = "127.0.0.1", port
        self.url = f"http://{self.host}:{port}"
        self.log = open(os.path.join(work, log_name), "wb")
        cmd = [sys.executable, os.path.join(REPO, "scripts", "fake_alpaca_camera.py"), "--host", self.host,
               "--port", str(port), *extra_args]
        print(f"e2e: starting {' '.join(cmd)}", flush=True)
        self.proc = subprocess.Popen(cmd, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while True:
            try:
                self.request("GET", "/fake/state")
                return
            except OSError:
                if self.proc.poll() is not None or time.time() > deadline:
                    raise Failure(f"the fake Alpaca server did not start on port {port}")
                time.sleep(0.2)

    def request(self, method, path, data=None):
        body = urllib.parse.urlencode(data).encode() if data is not None else None
        req = urllib.request.Request(self.url + path, data=body, method=method)
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read().decode())

    def inject(self, call, error, count, message):
        return self.request("PUT", "/fake/inject", {"call": call, "error": error, "count": count, "message": message})

    def truth(self):
        """GET /fake/truth: the camera angle, rates, backlash, drift and PE the guider should measure."""
        return self.request("GET", "/fake/truth")

    def clouds(self, frames, dim):
        return self.request("PUT", "/fake/clouds", {"frames": frames, "dim": dim})

    def telescope(self, prop):
        return self.request("GET", f"/api/v1/telescope/0/{prop}")["Value"]

    def slew(self, ra_hours, dec_deg):
        """Move the mount the way Ara does for a new target: straight to the Alpaca telescope, not via the guider."""
        self.request("PUT", "/api/v1/telescope/0/slewtocoordinatesasync",
                     {"RightAscension": ra_hours, "Declination": dec_deg, "ClientID": 1, "ClientTransactionID": 1})
        return self.request("GET", "/api/v1/telescope/0/declination")["Value"]

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


class Client:
    """A JSON-RPC connection on the daemon's :4400 port. A reader thread files the responses by id and records
    every event, in order, with its arrival time (and appends it to events.jsonl)."""

    def __init__(self, port, daemon, events_path, connect_timeout):
        self.daemon = daemon
        deadline = time.time() + connect_timeout
        while True:
            daemon.check_alive()
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                if time.time() > deadline:
                    raise Failure(f"the daemon's JSON-RPC port {port} did not open within {connect_timeout} s")
                time.sleep(0.5)
        self.sock.settimeout(None)
        self.events = []
        self.responses = {}
        self.cv = threading.Condition()
        self.closed = False
        self.next_id = 1
        self.events_file = open(events_path, "a")
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        try:
            for line in self.sock.makefile("rb"):
                msg = json.loads(line.decode("utf-8", errors="replace"))
                with self.cv:
                    if isinstance(msg, dict) and "Event" in msg:
                        msg["_t"] = time.time()
                        self.events.append(msg)
                        self.events_file.write(json.dumps(msg) + "\n")
                        self.events_file.flush()
                    elif isinstance(msg, dict):
                        self.responses[msg.get("id")] = msg
                    self.cv.notify_all()
        except (OSError, ValueError):
            pass
        with self.cv:
            self.closed = True
            self.cv.notify_all()

    def request(self, method, params=None, timeout=30):
        with self.cv:
            req_id = self.next_id
            self.next_id += 1
        req = {"jsonrpc": "2.0", "method": method, "id": req_id}
        if params is not None:
            req["params"] = params
        self.sock.sendall((json.dumps(req) + "\n").encode())
        deadline = time.time() + timeout
        with self.cv:
            while req_id not in self.responses:
                if self.closed:
                    self.daemon.check_alive()
                    raise Failure(f"the connection closed while waiting for {method}")
                left = deadline - time.time()
                if left <= 0:
                    raise Failure(f"no response to {method} within {timeout} s")
                self.cv.wait(min(left, 1.0))
            return self.responses.pop(req_id)

    def call(self, method, params=None, timeout=30):
        msg = self.request(method, params, timeout)
        if msg.get("error"):
            raise Failure(f"{method} failed: {msg['error']}")
        return msg.get("result")

    def mark(self):
        """The index of the next event; pass it to wait_events/events_since."""
        with self.cv:
            return len(self.events)

    def events_since(self, start, names=None):
        with self.cv:
            evs = self.events[start:]
        return [e for e in evs if names is None or e["Event"] in names]

    def wait_events(self, start, name, count=1, timeout=60, abort_on=()):
        """Wait until `count` events called `name` arrived since `start`; fail at once on an event in abort_on."""
        deadline = time.time() + timeout
        while True:
            evs = self.events_since(start)
            bad = [e for e in evs if e["Event"] in abort_on]
            if bad:
                raise Failure(f"{bad[0]['Event']} while waiting for {name}: {strip(bad[0])}")
            got = [e for e in evs if e["Event"] == name]
            if len(got) >= count:
                return got
            self.daemon.check_alive()
            expect(not self.closed, f"the connection closed while waiting for {name}")
            expect(time.time() < deadline, f"{len(got)} of {count} {name} events within {timeout} s")
            with self.cv:
                self.cv.wait(0.5)

    def wait_state(self, want, timeout=60):
        deadline = time.time() + timeout
        while True:
            state = self.call("get_app_state")
            if state == want:
                return
            expect(time.time() < deadline, f"app state {state}, expected {want} within {timeout} s")
            time.sleep(0.25)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
        self.events_file.close()


def strip(ev):
    return {k: v for k, v in ev.items() if k not in ("Timestamp", "Host", "Inst", "_t")}


SETTLE = {"pixels": 1.5, "time": 2, "timeout": 90}

# The physics lane's fake (#142): a camera at 37 deg, RA moving cos(Dec) as far, Dec backlash, a drift and RA
# periodic error in simulated time, seeing and a new noise background each frame; ImageBytes frames keep the
# fake's CPU low while the main lane runs beside it.
PHYSICS_CAMERA_ANGLE_DEG = 37.0
PHYSICS_DEC = 60.0
PHYSICS_SEEING_PX = 0.15
PHYSICS_FAKE_ARGS = (
    "--imagebytes", "--camera-angle", str(PHYSICS_CAMERA_ANGLE_DEG), "--cos-dec", "--dec-backlash-ms", "800",
    "--sim-drift", "0.2,0.3", "--pe-px", "0.3", "--pe-period-s", "300", "--seeing-px", str(PHYSICS_SEEING_PX),
    "--frame-noise", "--pier-flip", "--seed", "142",
)
# The physics calibration takes more steps than the plain fake's (cos(Dec) slows the RA legs, the backlash adds
# Dec steps): about 1.4 times as long, so its lane waits that much longer for CalibrationComplete.
PHYSICS_CALIBRATION_TIMEOUT_FACTOR = 1.5
TRUTH_ANGLE_DEG = 2.0  # calibration angles within this of the truth
TRUTH_RATE_FRACTION = 0.05  # calibration rates within this fraction of the truth
DITHER_AMOUNT = 3
TRUE_STAR_PX = 0.75  # the true star's move within this of the lock's, after a dither settled
CLOUD_FRAMES = 5
# a thick cloud: the guide star (40000 ADU) peaks at 0.04 ADU against the fake's 15 ADU noise, so the frame is
# noise only. (At 1000 the star peaks at 40 ADU and is still found at SNR 9-12 on about a third of the frames,
# which only the guider's mass-change check turns into a StarLost.)
CLOUD_DIM = 1e6
# What a frame with no star reads as (src/star.h FindResult, src/guider_multistar.cpp StarStatusStr): the
# smoothed peak of pure noise stays under the 3-sigma threshold, so the SNR is capped under 3 (low SNR), or no
# pixel at all is over the threshold (low mass). Not "mass changed" (7), which needs a star found, and not a
# camera error, which has no ErrorCode.
NO_STAR_ERRORS = {2: "Star lost - low SNR", 3: "Star lost - low mass"}
# the abort list of a guide request that must succeed
GUIDE_FAILURES = ("CalibrationFailed", "GuidingStopped", "LoopingExposuresStopped", "EquipmentDisconnected")


def check_guide_request_order(evs):
    """The event order of one `guide` request with recalibrate true, as design/API_CONTRACT.md documents it:
    StartCalibration, Calibrating..., CalibrationComplete, StartGuiding, SettleBegin, then Settling and GuideStep
    events until exactly one SettleDone, then GuideStep events."""
    names = [e["Event"] for e in evs if e["Event"] in CONTRACT_EVENTS]
    seq = " ".join(names) + " "
    pattern = (r"StartCalibration (Calibrating )+CalibrationComplete StartGuiding SettleBegin "
               r"((Settling|GuideStep) )*SettleDone (GuideStep )*")
    expect(re.fullmatch(pattern, seq), f"event order does not match the contract: {seq.strip()}")
    expect(names.count("SettleDone") == 1, f"{names.count('SettleDone')} SettleDone events for one guide request")
    return names


def guide_with_recalibration(cli, guide_steps_after, timeout):
    """Run one `guide` with recalibrate true to a settled guider and `guide_steps_after` GuideSteps past the
    SettleDone. Returns (the request's events, the SettleDone)."""
    start = cli.mark()
    expect(cli.call("guide", {"settle": SETTLE, "recalibrate": True}) == 0, "guide did not return 0")
    cli.wait_events(start, "StartCalibration", timeout=60, abort_on=GUIDE_FAILURES)
    cli.wait_events(start, "CalibrationComplete", timeout=timeout, abort_on=GUIDE_FAILURES)
    done = cli.wait_events(start, "SettleDone", timeout=SETTLE["timeout"] + 30, abort_on=GUIDE_FAILURES)[0]
    expect(done.get("Status") == 0, f"SettleDone {strip(done)}, expected Status 0")
    expect(not done.get("Error"), f"SettleDone has an error: {strip(done)}")
    expect(done.get("DroppedFrames") == 0, f"SettleDone dropped frames: {strip(done)}")
    after = cli.events_since(start).index(done) + start + 1
    cli.wait_events(after, "GuideStep", count=guide_steps_after, timeout=60, abort_on=GUIDE_FAILURES)
    evs = cli.events_since(start)
    expect(cli.call("get_app_state") == "Guiding", "not guiding after the settle")
    return evs, done


def calibration_summary(evs):
    """Calibrating steps per leg, by the event's "dir": West, East, Backlash, North, South or NudgeSouth.

    Not by "State", which is the translatable status message ("Clearing backlash step 1")."""
    legs = {}
    for e in evs:
        if e["Event"] == "Calibrating":
            leg = str(e.get("dir"))
            legs[leg] = legs.get(leg, 0) + 1
    return legs


def check_full_calibration(evs):
    """One full calibration: StartCalibration, the West, East, North and South legs, CalibrationComplete."""
    names = [e["Event"] for e in evs]
    expect(names.count("StartCalibration") == 1, f"{names.count('StartCalibration')} StartCalibration events")
    expect(names.count("CalibrationComplete") == 1, f"{names.count('CalibrationComplete')} CalibrationComplete")
    legs = calibration_summary(evs)
    for leg in ("West", "East", "North", "South"):
        expect(legs.get(leg, 0) > 0, f"no {leg} calibration steps: {legs}")
    return legs


def loop_and_find_star(cli):
    """loop, then find_star: the star the guide request calibrates on. Returns its position."""
    start = cli.mark()
    expect(cli.call("loop") == 0, "loop did not return 0")
    cli.wait_events(start, "LoopingExposures", count=3, timeout=60, abort_on=("LoopingExposuresStopped",))
    pos = cli.call("find_star")
    expect(isinstance(pos, list) and len(pos) == 2, f"find_star returned {pos}")
    cli.wait_events(start, "StarSelected", timeout=30)
    return pos


def scenario_calibrate_and_guide(cli, args):
    pos = loop_and_find_star(cli)

    evs, done = guide_with_recalibration(cli, args.guide_steps, args.calibration_timeout)
    legs = check_full_calibration(evs)
    names = check_guide_request_order(evs)
    cal = cli.call("get_calibration_data", {"which": "Mount"})
    expect(cal.get("calibrated") is True, f"not calibrated: {cal}")
    return (f"star at {pos}, calibration steps {legs}, settled in {done.get('TotalFrames')} frames, "
            f"{names.count('GuideStep')} GuideSteps; event order ok")


def scenario_new_target(cli, fake, args):
    """Ara's new-target sequence: stop guiding, slew, guide with recalibrate true. The guider calibrates again."""
    before = cli.call("get_calibration_data", {"which": "Mount"})
    cli.call("stop_capture")
    cli.wait_state("Stopped", 30)
    dec = fake.slew(15.5, 50.0)
    expect(abs(dec - 50.0) < 1e-6, f"the fake mount reads Dec {dec} after the slew")

    evs, done = guide_with_recalibration(cli, args.guide_steps, args.calibration_timeout)
    legs = check_full_calibration(evs)
    check_guide_request_order(evs)
    after = cli.call("get_calibration_data", {"which": "Mount"})
    expect(after.get("calibrated") is True, f"not calibrated: {after}")
    expect(abs(after.get("declination", 0) - 50.0) < 0.5,
           f"calibration declination {after.get('declination')}, expected 50 "
           f"(before the slew {before.get('declination')})")
    # the plain fake's sideofpier reads 0, pierEast; the calibration records it (#189)
    expect(after.get("pierSide") == "East", f"calibration pierSide {after.get('pierSide', '(missing)')!r}, "
                                            "expected 'East'")
    total = sum(1 for e in cli.events_since(0) if e["Event"] == "StartCalibration")
    expect(total >= 2, f"{total} StartCalibration events since connect, expected a second calibration")
    return (f"second calibration at Dec {after.get('declination')} (was {before.get('declination')}), "
            f"steps {legs}, settled in {done.get('TotalFrames')} frames")


def scenario_guide_history(cli):
    """#133: while guiding, each get_guide_history point is the GuideStep event taken at the same moment: "t" an
    integer millisecond UTC time within 0.2 s of the event's Timestamp (the guide exposure is 500 ms), "ra_dur" /
    "dec_dur" the event's RADuration / DECDuration (0 when absent), and "ra_dir" / "dec_dir" the first letter of
    its RADirection / DECDirection, or "" when the axis had no pulse."""
    expect(cli.call("get_app_state") == "Guiding", "not guiding before get_guide_history")
    hist = cli.call("get_guide_history", {"max_points": 400})
    points = hist.get("points") or []
    expect(len(points) >= 5, f"{len(points)} guide history points while guiding")
    steps = cli.events_since(0, ("GuideStep",))
    pulses = 0
    last_t = None
    for i, p in enumerate(points):
        t = p.get("t")
        expect(isinstance(t, int) and not isinstance(t, bool), f"point {i}: t {t!r} is not an integer: {p}")
        expect(last_t is None or t > last_t, f"point {i}: t {t} is not after the point before ({last_t})")
        last_t = t
        near = [e for e in steps if abs(e["Timestamp"] * 1000 - t) <= 200]
        expect(len(near) == 1, f"point {i} (t {t}): {len(near)} GuideStep events within 0.2 s, expected 1")
        ev = near[0]
        for axis, dur_key, dir_key in (("ra", "RADuration", "RADirection"), ("dec", "DECDuration", "DECDirection")):
            dur = ev.get(dur_key, 0)
            want = ev[dir_key][0] if dur > 0 else ""
            expect(p.get(f"{axis}_dur") == dur, f"point {i}: {axis}_dur {p.get(f'{axis}_dur')}, its GuideStep "
                   f"{dur_key} {dur}: {p} vs {strip(ev)}")
            expect(p.get(f"{axis}_dir") == want, f"point {i}: {axis}_dir {p.get(f'{axis}_dir')!r}, expected "
                   f"{want!r} from its GuideStep: {p} vs {strip(ev)}")
            pulses += 1 if want else 0
    expect(pulses > 0, f"no point in {len(points)} has a pulse direction")
    return f"{len(points)} points match their GuideStep events, {pulses} pulse directions"


def scenario_frame_drops(cli, fake, args):
    """#136: while guiding, 1 and then 2 frames in a row that fail with Alpaca 1280 are each a StarLost, and
    guiding goes on. A good frame between them starts a new count."""
    expect(cli.call("get_app_state") == "Guiding", "not guiding before the frame drops")
    results = []
    for drops in (1, 2):
        start = cli.mark()
        fake.inject("imagearray", 1280, drops, f"e2e injected acquisition failure x{drops}")
        lost = cli.wait_events(start, "StarLost", count=drops, timeout=60, abort_on=GUIDE_FAILURES)
        for e in lost:
            expect("1280" in str(e.get("Status", "")), f"StarLost without the Alpaca 1280 reason: {strip(e)}")
        last_lost = cli.events_since(start).index(lost[-1]) + start + 1
        cli.wait_events(last_lost, "GuideStep", count=3, timeout=60, abort_on=GUIDE_FAILURES)
        lost_now = cli.events_since(start, ("StarLost",))
        expect(len(lost_now) == drops, f"{len(lost_now)} StarLost events for {drops} dropped frames")
        expect(cli.call("get_app_state") == "Guiding", f"not guiding after {drops} dropped frames")
        results.append(f"{drops} drop(s): {len(lost_now)} StarLost, guiding")
    return "; ".join(results)


# #210: a single frame long enough to change the noise reduction while it is exposed (the camera sleeps all but the
# last 100 ms of it before polling imageready), and when to change it
NR_EXPOSURE_MS = 4000
NR_CHANGE_AFTER_S = 1.5
# the fake's background is Gaussian, sigma 15 ADU; a 3x3 median leaves about 6 of it. A frame is filtered when its
# robust background sigma is under this
NR_FILTERED_SIGMA = 11.0


def read_fits_u16(path):
    """The pixels of a 2-D 16-bit FITS file as written by usImage::Save (BITPIX 16, BZERO 32768), as a numpy
    array [y][x]."""
    with open(path, "rb") as f:
        return fits_u16_from_bytes(f.read(), path)


def background_sigma(pixels):
    """A robust sigma of the frame: 1.4826 x the median absolute deviation, which the few star pixels don't move."""
    import numpy as np

    med = np.median(pixels)
    return 1.4826 * float(np.median(np.abs(pixels - med)))


def single_frame_sigma(cli, during=None):
    """capture_single_frame {save: true}, `during` (if given) called NR_CHANGE_AFTER_S into the exposure; returns
    the saved frame's background sigma."""
    expect(cli.call("get_app_state") == "Stopped", "capture is not stopped before the single frame")
    start = cli.mark()
    cli.call("capture_single_frame", {"exposure": NR_EXPOSURE_MS, "save": True})
    if during is not None:
        time.sleep(NR_CHANGE_AFTER_S)
        during()
    done = cli.wait_events(start, "SingleFrameComplete", timeout=60)[0]
    expect(done.get("Success") is True and done.get("Path"), f"the single frame failed: {strip(done)}")
    return background_sigma(read_fits_u16(done["Path"]))


def scenario_noise_reduction_at_schedule(cli):
    """#210: the worker thread applies the noise reduction that was in effect when the frame was scheduled, carried
    with the exposure request. set_noise_reduction during the exposure takes effect from the next frame; the
    worker used to read the method from MyFrame after the capture, unsynchronized, and filter the frame in flight
    with the new one."""
    cli.call("set_noise_reduction", {"method": "None"})
    try:
        sigma_changed = single_frame_sigma(
            cli, lambda: cli.call("set_noise_reduction", {"method": "3x3 Median"}))
        expect(cli.call("get_noise_reduction") == "3x3 Median", "set_noise_reduction did not take")
        # the next frame is scheduled with the median, and comes back filtered: the measure tells them apart
        sigma_median = single_frame_sigma(cli)
    finally:
        cli.call("set_noise_reduction", {"method": "None"})
    expect(sigma_median < NR_FILTERED_SIGMA,
           f"a frame scheduled with the 3x3 median came back unfiltered: background sigma {sigma_median:.1f} ADU")
    expect(sigma_changed >= NR_FILTERED_SIGMA,
           f"a frame scheduled with no noise reduction came back filtered by the 3x3 median set "
           f"{NR_CHANGE_AFTER_S:g} s into its {NR_EXPOSURE_MS} ms exposure: background sigma {sigma_changed:.1f} ADU "
           f"(the next frame, scheduled with the median: {sigma_median:.1f})")
    return (f"none at schedule, median set during the exposure: sigma {sigma_changed:.1f} ADU (unfiltered); "
            f"next frame with the median: {sigma_median:.1f}")


def http_request(port, path, headers=None, method="GET", timeout=30):
    """One raw HTTP/1.1 request on the daemon's data port (8080 + instance - 1), the path sent byte for byte (no
    client-side ../ or %-escape normalising, so the traversal guard sees what a hostile client sends), sent at
    once after the connect, as every HTTP client does (#258). Returns (status, {lower-case header name: value},
    body)."""
    lines = [f"{method} {path} HTTP/1.1", "Host: 127.0.0.1", "Connection: close"]
    lines += [f"{k}: {v}" for k, v in (headers or {}).items()]
    raw = b""
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin-1"))
        status, hdrs, body_at = None, {}, None
        while True:
            chunk = s.recv(65536)
            if chunk:
                raw += chunk
            if body_at is None and b"\r\n\r\n" in raw:
                head, _, _ = raw.partition(b"\r\n\r\n")
                body_at = len(head) + 4
                status_line, *header_lines = head.decode("latin-1").split("\r\n")
                status = int(status_line.split()[1])
                for line in header_lines:
                    k, _, v = line.partition(":")
                    hdrs[k.strip().lower()] = v.strip()
            if body_at is not None:
                # a 304 has no body; otherwise stop at Content-Length rather than wait for the close
                if status == 304 or ("content-length" in hdrs and len(raw) - body_at >= int(hdrs["content-length"])):
                    break
            if not chunk:
                break
    expect(body_at is not None, f"GET {path}: no complete HTTP response header in {raw[:200]!r}")
    return status, hdrs, raw[body_at:]


def jpeg_segments(data):
    """The JPEG's header segments up to its first SOS: [(marker, payload)], and the offset of the scan data."""
    expect(data[:2] == b"\xff\xd8", f"not a JPEG: starts {data[:4]!r}")
    segs, i = [], 2
    while True:
        expect(i + 4 <= len(data) and data[i] == 0xFF, f"JPEG: no marker at byte {i}")
        while data[i] == 0xFF:
            i += 1
        marker = data[i]
        i += 1
        if marker == 0xD8 or 0xD0 <= marker <= 0xD7:
            continue
        length = int.from_bytes(data[i:i + 2], "big")
        segs.append((marker, data[i + 2:i + length]))
        i += length
        if marker == 0xDA:
            return segs, i


def jpeg_sof(data):
    """The JPEG's frame header, stdlib only: {"marker", "precision", "height", "width", "components": [(id, h, v,
    table)]}. Fails unless it is a single SOF."""
    segs, _ = jpeg_segments(data)
    sofs = [(m, p) for m, p in segs if 0xC0 <= m <= 0xCF and m not in (0xC4, 0xC8, 0xCC)]
    expect(len(sofs) == 1, f"JPEG: {len(sofs)} SOF segments")
    marker, p = sofs[0]
    n = p[5]
    comps = [(p[6 + 3 * k], p[7 + 3 * k] >> 4, p[7 + 3 * k] & 15, p[8 + 3 * k]) for k in range(n)]
    return {"marker": marker, "precision": p[0], "height": int.from_bytes(p[1:3], "big"),
            "width": int.from_bytes(p[3:5], "big"), "components": comps}


JPEG_ZIGZAG = (
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63,
)


def jpeg_decode_luma(data):
    """The first component (Y) of a baseline (SOF0) Huffman JPEG as a numpy uint8 array [y][x]: a small decoder
    so the check needs no PIL. For the guide frame, which is grey (R = G = B), Y is the grey level."""
    import numpy as np

    segs, pos = jpeg_segments(data)
    sof = jpeg_sof(data)
    expect(sof["marker"] == 0xC0 and sof["precision"] == 8, f"not a baseline 8-bit JPEG: {sof}")
    qt, ht, restart = {}, {}, 0
    for marker, p in segs:
        if marker == 0xDB:
            j = 0
            while j < len(p):
                prec, tid = p[j] >> 4, p[j] & 15
                size = 2 if prec else 1
                vals = [int.from_bytes(p[j + 1 + size * k:j + 1 + size * (k + 1)], "big") for k in range(64)]
                q = np.zeros(64)
                q[list(JPEG_ZIGZAG)] = vals
                qt[tid] = q.reshape(8, 8)
                j += 1 + 64 * size
        elif marker == 0xC4:
            j = 0
            while j < len(p):
                cls, tid = p[j] >> 4, p[j] & 15
                counts = p[j + 1:j + 17]
                syms = p[j + 17:j + 17 + sum(counts)]
                table, code, k = {}, 0, 0
                for length in range(1, 17):
                    for _ in range(counts[length - 1]):
                        table[format(code, f"0{length}b")] = syms[k]
                        code += 1
                        k += 1
                    code <<= 1
                ht[(cls, tid)] = table
                j += 17 + sum(counts)
        elif marker == 0xDD:
            restart = int.from_bytes(p[:2], "big")
    sos = segs[-1][1]
    scan = [(sos[1 + 2 * k], sos[2 + 2 * k] >> 4, sos[2 + 2 * k] & 15) for k in range(sos[0])]
    comps = {cid: (h, v, tq) for cid, h, v, tq in sof["components"]}
    expect(len(scan) == len(comps), "JPEG: the scan is not interleaved over every component")
    hmax = max(h for h, _, _ in comps.values())
    vmax = max(v for _, v, _ in comps.values())
    mcus_x = -(-sof["width"] // (8 * hmax))
    mcus_y = -(-sof["height"] // (8 * vmax))

    # the entropy-coded data, split at the restart markers, with the 0xFF00 stuffing taken out
    parts, cur, i = [], bytearray(), pos
    while i < len(data):
        b = data[i]
        if b == 0xFF and i + 1 < len(data):
            nxt = data[i + 1]
            if nxt == 0x00:
                cur.append(0xFF)
                i += 2
                continue
            if 0xD0 <= nxt <= 0xD7:
                parts.append(bytes(cur))
                cur = bytearray()
                i += 2
                continue
            break  # EOI or another marker: the scan ends
        cur.append(b)
        i += 1
    parts.append(bytes(cur))

    y_id = sof["components"][0][0]
    yh, yv, ytq = comps[y_id]
    coefs = np.zeros((mcus_y * yv, mcus_x * yh, 64))
    mcus_per_part = restart or mcus_x * mcus_y
    for part_no, part in enumerate(parts):
        bits = "".join(format(b, "08b") for b in part)
        bp = 0
        pred = {cid: 0 for cid, _, _ in scan}

        def huff(table):
            nonlocal bp
            for length in range(1, 17):
                sym = table.get(bits[bp:bp + length])
                if sym is not None:
                    bp += length
                    return sym
            raise Failure(f"JPEG: bad Huffman code at bit {bp}")

        def receive(n):
            nonlocal bp
            if n == 0:
                return 0
            v = int(bits[bp:bp + n], 2)
            bp += n
            return v - (1 << n) + 1 if v < (1 << (n - 1)) else v

        first = part_no * mcus_per_part
        for m in range(first, min(first + mcus_per_part, mcus_x * mcus_y)):
            my, mx = divmod(m, mcus_x)
            for cid, td, ta in scan:
                h, v, _ = comps[cid]
                for by in range(v):
                    for bx in range(h):
                        block = np.zeros(64) if cid == y_id else None
                        pred[cid] += receive(huff(ht[(0, td)]))
                        if block is not None:
                            block[0] = pred[cid]
                        k = 1
                        while k < 64:
                            rs = huff(ht[(1, ta)])
                            r, s = rs >> 4, rs & 15
                            if s == 0:
                                if r != 15:
                                    break
                                k += 16
                                continue
                            k += r
                            val = receive(s)
                            if block is not None:
                                block[JPEG_ZIGZAG[k]] = val
                            k += 1
                        if block is not None:
                            coefs[my * v + by, mx * h + bx] = block
    # dequantize, then the 8x8 inverse DCT of every block at once: f = C^T F C
    n = np.arange(8)
    c = np.cos((2 * n[None, :] + 1) * n[:, None] * np.pi / 16) / 2
    c[0] /= math.sqrt(2)
    blocks = coefs.reshape(*coefs.shape[:2], 8, 8) * qt[ytq]
    pix = np.einsum("ux,abuv,vy->abxy", c, blocks, c) + 128.0
    img = pix.transpose(0, 2, 1, 3).reshape(coefs.shape[0] * 8, coefs.shape[1] * 8)
    img = img[:sof["height"] * yv // vmax, :sof["width"] * yh // hmax]
    return np.clip(np.round(img), 0, 255).astype(np.uint8)


def fits_u16_from_bytes(data, what):
    """read_fits_u16 on bytes already read (`what` names them in a failure)."""
    import numpy as np

    cards, off = {}, 0
    while True:
        block = data[off:off + 2880]
        expect(len(block) == 2880, f"{what}: the FITS header has no END card")
        off += 2880
        done = False
        for i in range(0, 2880, 80):
            card = block[i:i + 80].decode("ascii", errors="replace")
            if card[:8].strip() == "END":
                done = True
                break
            if card[8:10] == "= ":
                cards[card[:8].strip()] = card[10:].split("/")[0].strip()
        if done:
            break
    expect(cards.get("BITPIX") == "16" and cards.get("NAXIS") == "2", f"{what}: not a 2-D 16-bit FITS ({cards})")
    w, h = int(cards["NAXIS1"]), int(cards["NAXIS2"])
    raw = np.frombuffer(data, dtype=">i2", count=w * h, offset=off).astype(np.float64)
    return (raw * float(cards.get("BSCALE", "1")) + float(cards.get("BZERO", "0"))).reshape(h, w)


def display_stretch(pixels, gamma=1.0):
    """What /api/frame.jpg paints, from the frame's pixels: usImage::CalcStats' FiltMin / FiltMax (the min and max
    after a 3x3 median; the edge pixels, which Median3 takes over 4 or 6 neighbours, are left out here) through
    CopyToImage's gamma table (buildGammaLookupTable: 0 at or under FiltMin, 255 at or over FiltMax, truncated).
    The profile's default gamma is 1.0 (GAMMA_DEFAULT; the Gamma line of UpdateImageDisplay in the debug log)."""
    import numpy as np

    h, w = pixels.shape
    shifted = [pixels[1 + dy:h - 1 + dy, 1 + dx:w - 1 + dx] for dy in (-1, 0, 1) for dx in (-1, 0, 1)]
    med = np.median(np.stack(shifted), axis=0)
    lo, hi = float(med.min()), float(med.max())
    d = np.clip((pixels - lo) / (hi - lo), 0.0, 1.0)
    return np.floor((d ** gamma) * 255.0).astype(np.uint8), lo, hi


# the fake's default star field (scripts/fake_alpaca_camera.py STARS), its brightest star at (320, 240): the
# guide frame must show each one where the fake put it, after the field's move (pulses, from the smoke run)
FAKE_STARS = ((80, 60, 30000), (200, 120, 25000), (320, 240, 40000), (500, 100, 20000), (120, 350, 35000),
              (420, 380, 28000), (560, 300, 22000), (250, 420, 18000), (380, 60, 26000), (60, 250, 32000),
              (520, 430, 24000), (300, 160, 15000))
FAKE_SIZE = (640, 480)
FAKE_BACKGROUND_ADU = 800.0  # the fake's background, Gaussian, sigma 15 ADU
# The JPEG (quality 85) against the stretch of the same frame's FITS: its mean, and the 50th, 99th and 99.9th
# percentiles, in 8-bit levels. The plain fake's frame is near black under the default stretch (the background at
# 800 ADU is 0 over a FiltMin of about 771, FiltMax 34000-36000), so the mean is the stars': measured locally, 0.19
# to 0.20 against 0.18 to 0.19, the percentiles 0, 0 and 65-69 against 0, 0 and 66-69; the FiltMin/FiltMax computed
# here match the debug log's.
JPEG_MEAN_TOL = 0.1
JPEG_PERCENTILE_TOL = 3
JPEG_PERCENTILES = (50, 99, 99.9)
# a star of the field is painted at least this bright (8-bit) in the 3x3 px around where the fake put it; the
# faintest (15000 ADU) peaks at about 108 under the default stretch
JPEG_STAR_LEVEL = 40

CAPTURE_TRAVERSAL = (
    "../{name}", "..%2f{name}", "%2e%2e/{name}", "%2e%2e%2f{name}", "x/../{name}", "x%2f..%2f{name}",
    "..\\{name}", "/{name}", "%2f{name}", "\\{name}", "../../../../etc/passwd", "%2e%2e%2f%2e%2e%2fetc%2fpasswd",
    "", "no_such_file.fits",
)


def check_not_found(port, path, method="GET"):
    status, hdrs, body = http_request(port, path, method=method)
    expect((status, body) == (404, b"Not found"),
           f"{method} {path}: {status} {body[:80]!r}, expected 404 'Not found'")
    expect(hdrs.get("content-type") == "text/plain; charset=utf-8", f"{method} {path}: Content-Type {hdrs}")


def scenario_http_before_first_frame(args):
    """N2: the HTTP data port before the daemon has taken a frame. /api/frame.jpg is 404 'no image available'
    (text/plain), with or without an If-None-Match; any other path, a non-GET and a capture name the traversal
    guard rejects are 404 'Not found'."""
    port = args.http_port
    for hdrs in (None, {"If-None-Match": '"f0"'}):
        status, h, body = http_request(port, "/api/frame.jpg", headers=hdrs)
        expect((status, body) == (404, b"no image available"),
               f"/api/frame.jpg before the first frame ({hdrs}): {status} {body[:80]!r}, expected 404 "
               "'no image available'")
        expect(h.get("content-type") == "text/plain" and h.get("content-length") == str(len(body)),
               f"/api/frame.jpg before the first frame: headers {h}")
        expect("etag" not in h, f"/api/frame.jpg before the first frame has an ETag: {h}")
    for path in ("/", "/index.html", "/api/frame.jpeg", "/api/frame.jpg/", "/api/capture"):
        check_not_found(port, path)
    check_not_found(port, "/api/frame.jpg", method="POST")
    for name in CAPTURE_TRAVERSAL:
        check_not_found(port, "/api/capture/" + name.format(name="x.fits"))
    return (f"frame.jpg 404 'no image available' on :{port}; 6 other routes and {len(CAPTURE_TRAVERSAL)} "
            "capture names 404 'Not found'")


def single_frame(cli, exposure=500):
    start = cli.mark()
    cli.call("capture_single_frame", {"exposure": exposure})
    done = cli.wait_events(start, "SingleFrameComplete", timeout=60)[0]
    expect(done.get("Success") is True, f"the single frame failed: {strip(done)}")


def scenario_http_frame_and_capture(cli, args, work):
    """N2: /api/frame.jpg and /api/capture as Ara's live view and polar alignment use them, pinned before S5.3 and
    S7.1-S7.3 replace the painting code and the wxImage JPEG path behind them. On a stopped capture, after one
    single frame: frame.jpg is 200 image/jpeg with Content-Length, Cache-Control no-store and an ETag "f<N>"
    (the frame counter), 304 with the ETag and no body on a matching If-None-Match, 200 on another one, and
    "f<N+1>" after the next frame. The JPEG is a baseline SOF0 of the frame's size with 3 components; decoded,
    its grey level matches the FITS of the same frame through the display stretch (mean and percentiles), and
    each of the fake's stars is where the fake put it. /api/capture/<name> serves the file save_image wrote,
    byte for byte, as application/fits; the traversal guard answers 404 for ../-style names of that file, a
    POST, and a symlink in the capture dir to a file outside it."""
    import numpy as np

    port = args.http_port
    expect(cli.call("get_app_state") == "Stopped", "capture is not stopped before the frame.jpg check")
    cli.call("set_noise_reduction", {"method": "None"})
    single_frame(cli)

    status, h, jpeg = http_request(port, "/api/frame.jpg")
    expect(status == 200, f"/api/frame.jpg after a frame: {status} {jpeg[:80]!r}")
    expect(h.get("content-type") == "image/jpeg", f"/api/frame.jpg Content-Type {h.get('content-type')!r}")
    expect(h.get("content-length") == str(len(jpeg)) and jpeg,
           f"/api/frame.jpg Content-Length {h.get('content-length')!r} for {len(jpeg)} bytes")
    expect(h.get("cache-control") == "no-store", f"/api/frame.jpg Cache-Control {h.get('cache-control')!r}")
    etag = h.get("etag", "")
    m = re.fullmatch(r'"f(\d+)"', etag)
    expect(m, f"/api/frame.jpg ETag {etag!r}, expected \"f<N>\"")
    frame_no = int(m.group(1))
    expect(jpeg[-2:] == b"\xff\xd9", f"the JPEG does not end with EOI: {jpeg[-4:]!r}")

    status, h304, body = http_request(port, "/api/frame.jpg", headers={"If-None-Match": etag})
    expect(status == 304 and body == b"" and h304.get("etag") == etag,
           f"If-None-Match {etag}: {status}, ETag {h304.get('etag')!r}, {len(body)} body bytes; expected 304, the "
           "same ETag, no body")
    other = f'"f{frame_no + 1000}"'
    status, h2, jpeg2 = http_request(port, "/api/frame.jpg", headers={"If-None-Match": other})
    expect(status == 200 and h2.get("etag") == etag, f"If-None-Match {other}: {status}, ETag {h2.get('etag')!r}")

    sof = jpeg_sof(jpeg)
    expect(sof["marker"] == 0xC0 and sof["precision"] == 8, f"the JPEG is not baseline 8-bit (SOF0): {sof}")
    expect((sof["width"], sof["height"]) == FAKE_SIZE, f"JPEG {sof['width']}x{sof['height']}, the fake's frame is "
                                                       f"{FAKE_SIZE[0]}x{FAKE_SIZE[1]}")
    expect(len(sof["components"]) == 3, f"JPEG with {len(sof['components'])} components, expected 3")

    # save_image writes the same frame (the ETag still answers 304 after it), and /api/capture serves the file
    fname = cli.call("save_image")["filename"]
    status, _, _ = http_request(port, "/api/frame.jpg", headers={"If-None-Match": etag})
    expect(status == 304, f"the frame changed under save_image: If-None-Match {etag} is {status}")
    name = os.path.basename(fname)
    with open(fname, "rb") as f:
        on_disk = f.read()
    status, hc, fits = http_request(port, "/api/capture/" + urllib.parse.quote(name))
    expect(status == 200, f"/api/capture/{name}: {status} {fits[:80]!r}")
    expect(hc.get("content-type") == "application/fits" and hc.get("content-length") == str(len(fits)),
           f"/api/capture/{name}: headers {hc}")
    expect(hc.get("cache-control") == "no-store" and "etag" not in hc, f"/api/capture/{name}: headers {hc}")
    expect(fits == on_disk, f"/api/capture/{name}: {len(fits)} bytes, not the {len(on_disk)} bytes save_image wrote")
    pixels = fits_u16_from_bytes(fits, name)
    expect(pixels.shape == (sof["height"], sof["width"]), f"FITS {pixels.shape}, JPEG {sof['width']}x{sof['height']}")
    bg = float(np.median(pixels))
    expect(abs(bg - FAKE_BACKGROUND_ADU) < 3, f"the frame's median {bg:.1f} ADU, the fake's background is 800")

    gray = jpeg_decode_luma(jpeg)
    expect(gray.shape == pixels.shape, f"decoded JPEG {gray.shape}, FITS {pixels.shape}")
    want, lo, hi = display_stretch(pixels)
    got_p = [float(np.percentile(gray, q)) for q in JPEG_PERCENTILES]
    want_p = [float(np.percentile(want, q)) for q in JPEG_PERCENTILES]
    stats = (f"mean {gray.mean():.2f} (stretch of the FITS {want.mean():.2f}), percentiles {JPEG_PERCENTILES} "
             f"{got_p} ({want_p}), FiltMin/Max ~{lo:.0f}/{hi:.0f}")
    expect(abs(float(gray.mean()) - float(want.mean())) <= JPEG_MEAN_TOL, f"the JPEG's mean is off: {stats}")
    for q, g, w in zip(JPEG_PERCENTILES, got_p, want_p):
        expect(abs(g - w) <= JPEG_PERCENTILE_TOL, f"the JPEG's {q}th percentile is off: {stats}")
    # the stars, where the fake put them: the field moved as a whole, by where the brightest star now is
    by, bx = np.unravel_index(np.argmax(pixels), pixels.shape)
    ox, oy = int(bx) - 320, int(by) - 240
    expect(abs(ox) < 40 and abs(oy) < 40, f"the brightest pixel at ({bx}, {by}), not near the fake's (320, 240)")
    gy, gx = np.unravel_index(np.argmax(gray), gray.shape)
    expect(abs(int(gx) - int(bx)) <= 2 and abs(int(gy) - int(by)) <= 2,
           f"the JPEG's brightest pixel at ({gx}, {gy}), the FITS's at ({bx}, {by})")
    seen = 0
    for sx, sy, _ in FAKE_STARS:
        x, y = sx + ox, sy + oy
        if 2 <= x < FAKE_SIZE[0] - 2 and 2 <= y < FAKE_SIZE[1] - 2:
            peak = int(gray[y - 1:y + 2, x - 1:x + 2].max())
            expect(peak >= JPEG_STAR_LEVEL, f"no star in the JPEG at ({x}, {y}) (the fake's ({sx}, {sy}) moved by "
                                            f"({ox}, {oy})): peak {peak}")
            seen += 1
    expect(seen >= 10, f"only {seen} of the fake's stars inside the frame")

    # the traversal guard, on names that would reach the real file
    captured = os.path.dirname(fname)
    up = os.path.basename(captured)
    for pat in CAPTURE_TRAVERSAL[:6] + (f"../{up}/{{name}}", f"%2e%2e%2f{up}%2f{{name}}", "./../" + up + "/{name}"):
        check_not_found(port, "/api/capture/" + pat.format(name=name))
    check_not_found(port, "/api/capture/" + name, method="POST")
    outside = os.path.join(work, "n2_outside.fits")
    shutil.copy(fname, outside)
    link = os.path.join(captured, "n2_link.fits")
    os.symlink(outside, link)
    try:
        check_not_found(port, "/api/capture/n2_link.fits")
    finally:
        os.unlink(link)

    # A suspected bug, pinned as it is (N2): the ETag is the frame counter, which StartCapturing sets back to 0
    # for every capture (and NotifyGuidingStarted when guiding starts), so each single frame is "f1": the next
    # one, a new exposure (save_image's DATE-OBS moves on), still answers 304 to If-None-Match "f1", and a client
    # that caches by ETag keeps the old frame. Within a loop the counter goes up, so the ETag changes each frame.
    obs1 = fits_card(on_disk, "DATE-OBS")
    single_frame(cli)
    with open(cli.call("save_image")["filename"], "rb") as f:
        obs2 = fits_card(f.read(), "DATE-OBS")
    expect(obs1 and obs2 and obs1 != obs2, f"the next single frame's DATE-OBS {obs2!r}, the first's {obs1!r}")
    status, h3, _ = http_request(port, "/api/frame.jpg", headers={"If-None-Match": etag})
    expect(etag == '"f1"' and status == 304 and h3.get("etag") == etag,
           f"the next single frame (DATE-OBS {obs2} after {obs1}) with If-None-Match {etag}: {status}, ETag "
           f"{h3.get('etag')!r}; today each single frame is \"f1\" and answers 304 (the counter restarts)")
    start = cli.mark()
    expect(cli.call("loop") == 0, "loop did not return 0")
    try:
        cli.wait_events(start, "LoopingExposures", count=2, timeout=60, abort_on=("LoopingExposuresStopped",))
        _, h4, _ = http_request(port, "/api/frame.jpg")
        mark = cli.mark()
        cli.wait_events(mark, "LoopingExposures", count=2, timeout=60, abort_on=("LoopingExposuresStopped",))
        _, h5, _ = http_request(port, "/api/frame.jpg")
    finally:
        cli.call("stop_capture")
        cli.wait_state("Stopped", 30)
    n4, n5 = (int(re.fullmatch(r'"f(\d+)"', h.get("etag", '"f-1"')).group(1)) for h in (h4, h5))
    expect(n5 > n4 >= 1, f"while looping, the ETag went from {h4.get('etag')!r} to {h5.get('etag')!r}")
    return (f"ETag {etag}, 304 on a match; SOF0 {sof['width']}x{sof['height']}x3, {len(jpeg)} bytes; {stats}; "
            f"{seen} stars at the fake's places (moved {ox}, {oy}); /api/capture served {name} ({len(fits)} bytes), "
            f"traversal 404; the next single frame is {etag} again (304), looping {h4.get('etag')} -> "
            f"{h5.get('etag')}")


def fits_card(data, key):
    """The value of a FITS header card (quotes and comment taken off), or None."""
    for off in range(0, len(data), 80):
        card = data[off:off + 80].decode("ascii", errors="replace")
        if card[:8].strip() == "END":
            return None
        if card[:8].strip() == key and card[8:10] == "= ":
            return card[10:].split("/")[0].strip().strip("'").strip()
    return None


def angle_diff(a, b):
    """a - b in degrees, in (-180, 180]."""
    d = (a - b) % 360.0
    return d - 360.0 if d > 180.0 else d


def scenario_calibration_truth(cli, fake, args):
    """The calibration measures the fake's true camera angle and rates: a camera at PHYSICS_CAMERA_ANGLE_DEG, the
    mount at Dec PHYSICS_DEC (RA moving cos(Dec) as far) with Dec backlash, seeing, drift and periodic error. The
    guider's xAngle/yAngle must be within TRUTH_ANGLE_DEG of the truth and xRate/yRate within TRUTH_RATE_FRACTION."""
    # a target an hour west of the meridian, so the slew keeps the mount on its side of the pier
    lst = fake.telescope("siderealtime")
    dec = fake.slew((lst - 1.0) % 24.0, PHYSICS_DEC)
    expect(abs(dec - PHYSICS_DEC) < 1e-6, f"the fake mount reads Dec {dec} after the slew")
    pos = loop_and_find_star(cli)

    evs, done = guide_with_recalibration(cli, args.guide_steps, args.calibration_timeout)
    legs = check_full_calibration(evs)
    check_guide_request_order(evs)
    cal = cli.call("get_calibration_data", {"which": "Mount"})
    truth = fake.truth()
    expect(cal.get("calibrated") is True, f"not calibrated: {cal}")
    expect(abs(cal.get("declination", 0) - PHYSICS_DEC) < 0.5, f"calibration declination {cal.get('declination')}")
    # get_calibration_data reports the rates in px/s, /fake/truth in px/ms
    want = {"xAngle": truth["x_angle_deg"], "yAngle": truth["y_angle_deg"],
            "xRate": truth["ra_rate_px_per_ms"] * 1000.0, "yRate": truth["dec_rate_px_per_ms"] * 1000.0}
    errs = {"xAngle": angle_diff(cal["xAngle"], want["xAngle"]), "yAngle": angle_diff(cal["yAngle"], want["yAngle"]),
            "xRate": cal["xRate"] / want["xRate"] - 1.0, "yRate": cal["yRate"] / want["yRate"] - 1.0}
    got = ", ".join(f"{k} {cal[k]} (truth {want[k]:.4g}, {'off' if k.endswith('Angle') else 'error'} "
                    f"{errs[k]:+.2f}{' deg' if k.endswith('Angle') else ''})" for k in want)
    for k in ("xAngle", "yAngle"):
        expect(abs(errs[k]) <= TRUTH_ANGLE_DEG, f"{k} off the truth by more than {TRUTH_ANGLE_DEG} deg: {got}")
    for k in ("xRate", "yRate"):
        expect(abs(errs[k]) <= TRUTH_RATE_FRACTION, f"{k} off the truth by more than "
                                                   f"{TRUTH_RATE_FRACTION:.0%}: {got}")
    return (f"star at {pos}, calibration steps {legs}, {got}; settled in {done.get('TotalFrames')} frames")


def steps_between(evs, first, last):
    """The GuideStep events after evs[first] and before evs[last]."""
    return [e for e in evs[first + 1:last] if e["Event"] == "GuideStep"]


def scenario_dither_and_settle(cli, fake, args):
    """A `dither` of DITHER_AMOUNT px with a settle while guiding: GuidingDithered moves the lock by the dither
    mapped through the fake's true camera angles, exactly one SettleDone with Status 0, the GuideSteps start
    the lock's move away from the new lock and are corrected to within the settle pixels, and the true star
    (the fake's field offset) followed the lock."""
    expect(cli.call("get_app_state") == "Guiding", "not guiding before the dither")
    lock0 = cli.call("get_lock_position")
    truth0 = fake.truth()
    field0 = truth0["field_offset_px"]
    start = cli.mark()
    expect(cli.call("dither", {"amount": DITHER_AMOUNT, "raOnly": False, "settle": SETTLE}) == 0,
           "dither did not return 0")
    dithered = cli.wait_events(start, "GuidingDithered", timeout=30, abort_on=GUIDE_FAILURES)[0]
    done = cli.wait_events(start, "SettleDone", timeout=SETTLE["timeout"] + 30, abort_on=GUIDE_FAILURES)[0]
    expect(done.get("Status") == 0, f"SettleDone {strip(done)}, expected Status 0")
    expect(not done.get("Error"), f"SettleDone has an error: {strip(done)}")
    evs = cli.events_since(start)
    after = evs.index(done) + start + 1
    cli.wait_events(after, "GuideStep", count=3, timeout=60, abort_on=GUIDE_FAILURES)
    lock1 = cli.call("get_lock_position")
    field1 = fake.truth()["field_offset_px"]
    evs = cli.events_since(start)
    names = [e["Event"] for e in evs]
    expect(names.count("SettleDone") == 1, f"{names.count('SettleDone')} SettleDone events for one dither")
    expect(names.count("GuidingDithered") == 1, f"{names.count('GuidingDithered')} GuidingDithered events")
    expect(cli.call("get_app_state") == "Guiding", "not guiding after the dither settled")
    # the order Ara consumes: the lock moves, then the settle as for a guide request
    seq = " ".join(n for n in names if n in CONTRACT_EVENTS + ("LockPositionSet", "GuidingDithered")) + " "
    expect(re.fullmatch(r"(GuideStep )*LockPositionSet GuidingDithered SettleBegin ((Settling|GuideStep) )*"
                        r"SettleDone (GuideStep )*", seq), f"dither event order: {seq.strip()}")

    d_ra, d_dec = dithered["dx"], dithered["dy"]
    expect(max(abs(d_ra), abs(d_dec)) <= DITHER_AMOUNT + 1e-6, f"dither {d_ra}, {d_dec} over {DITHER_AMOUNT} px")
    lock_dx, lock_dy = lock1[0] - lock0[0], lock1[1] - lock0[1]
    lock_move = math.hypot(lock_dx, lock_dy)
    # The dither is in mount axes (RA, Dec px), the lock in camera px. The guider maps one onto the other with
    # its calibration (MountToCamera in src/calibration_transform.cpp): dRa along xAngle and dDec along yAngle,
    # as a rotation by xAngle that keeps the length, mirrored when yAngle is on the other side. So with the
    # fake's true angles the lock must move by that vector, to within the rotation the calibration's xAngle may
    # be off by (TRUTH_ANGLE_DEG). An RA dither landing on the Dec axis, or a mirror the wrong way, misses it by
    # about the dither's length. (MoveLockPosition reflects the dither near the frame's edge; the star is not.)
    # The dither is random and differs from run to run (#173), so it can be well under a pixel, where the rotation
    # allows next to nothing: the tolerance also allows the rounding, get_lock_position's 0.01 px on each
    # coordinate (so up to 0.01 px per axis on the move, 0.0142 px together) and GuidingDithered's 0.001 px.
    xa, ya = math.radians(truth0["x_angle_deg"]), math.radians(truth0["y_angle_deg"])
    want_dx = d_ra * math.cos(xa) + d_dec * math.cos(ya)
    want_dy = d_ra * math.sin(xa) + d_dec * math.sin(ya)
    lock_miss = math.hypot(lock_dx - want_dx, lock_dy - want_dy)
    lock_tol = 2.0 * math.sin(math.radians(TRUTH_ANGLE_DEG) / 2.0) * math.hypot(d_ra, d_dec) + 0.015
    expect(lock_miss <= lock_tol,
           f"the lock moved ({lock_dx:+.2f}, {lock_dy:+.2f}) px for a dither of RA {d_ra:+.2f} Dec {d_dec:+.2f} px, "
           f"expected ({want_dx:+.2f}, {want_dy:+.2f}) from the true angles: {lock_miss:.3f} px off (at most "
           f"{lock_tol:.3f})")
    steps = steps_between(evs, names.index("GuidingDithered"), names.index("SettleDone"))
    expect(len(steps) >= 2, f"{len(steps)} GuideSteps between the dither and SettleDone")
    first, last = steps[0], steps[-1]
    first_dist, last_dist = math.hypot(first["dx"], first["dy"]), math.hypot(last["dx"], last["dy"])
    # the first step sees the star where it was, so (dx, dy) = star - lock is about minus the lock's move (plus
    # the guiding error before it)
    first_miss = math.hypot(first["dx"] + lock_dx, first["dy"] + lock_dy)
    expect(first_miss < 1.0,
           f"first GuideStep after the dither at ({first['dx']:+.2f}, {first['dy']:+.2f}) px from the lock, the lock "
           f"moved ({lock_dx:+.2f}, {lock_dy:+.2f}): {first_miss:.2f} px off")
    # (a dither under a pixel may sit inside the algorithms' minimum move)
    expect(lock_move < 1.0 or first.get("RADuration", 0) > 0 or first.get("DECDuration", 0) > 0,
           f"no correction on the first GuideStep after a {lock_move:.2f} px dither: {strip(first)}")
    expect(last_dist <= SETTLE["pixels"], f"last GuideStep before SettleDone {last_dist:.2f} px from the lock")
    # the true star went where the lock went: the fake's field moved by the lock's move, as a vector, to within
    # the guiding error before the dither and after the settle (a few tenths of a px with this seeing, drift and
    # PE), well under the lock's move, so a star that stayed put or moved elsewhere fails
    star_dx, star_dy = field1["x"] - field0["x"], field1["y"] - field0["y"]
    off = math.hypot(star_dx - lock_dx, star_dy - lock_dy)
    expect(off <= TRUE_STAR_PX,
           f"the true star moved ({star_dx:+.2f}, {star_dy:+.2f}) px, the lock ({lock_dx:+.2f}, {lock_dy:+.2f}): "
           f"{off:.2f} px apart (at most {TRUE_STAR_PX})")
    dists = " ".join(f"{math.hypot(e['dx'], e['dy']):.2f}" for e in steps)
    return (f"dither RA {d_ra:+.2f} Dec {d_dec:+.2f} px, lock moved ({lock_dx:+.2f}, {lock_dy:+.2f}) px, "
            f"{lock_miss:.3f} px from the true angles' ({want_dx:+.2f}, {want_dy:+.2f}); GuideSteps {dists} px to "
            f"SettleDone in {done.get('TotalFrames')} frames; true star {off:.2f} px from the lock's move")


def scenario_clouds(cli, fake, args):
    """Clouds while guiding: CLOUD_FRAMES frames with no star (CLOUD_DIM) are one StarLost each, each one the
    guider finding no star (NO_STAR_ERRORS), guiding is not stopped (get_app_state reads LostLock or Guiding
    meanwhile, doc/jsonrpc_api.md), and the star is found again on the first clear frame, with the GuideSteps
    going on after it."""
    expect(cli.call("get_app_state") == "Guiding", "not guiding before the clouds")
    start = cli.mark()
    fake.clouds(CLOUD_FRAMES, CLOUD_DIM)
    states = set()
    deadline = time.time() + 60
    while len(cli.events_since(start, ("StarLost",))) < CLOUD_FRAMES:
        states.add(cli.call("get_app_state"))
        bad = cli.events_since(start, GUIDE_FAILURES)
        expect(not bad, f"{bad[0]['Event'] if bad else ''} while clouded: {strip(bad[0]) if bad else ''}")
        expect(time.time() < deadline, f"{len(cli.events_since(start, ('StarLost',)))} of {CLOUD_FRAMES} StarLost "
                                       f"events within 60 s of the clouds")
        time.sleep(0.2)
    lost = cli.events_since(start, ("StarLost",))
    last_lost = cli.events_since(start).index(lost[-1]) + start + 1
    resumed = cli.wait_events(last_lost, "GuideStep", count=5, timeout=60, abort_on=GUIDE_FAILURES)
    lost_now = cli.events_since(start, ("StarLost",))
    expect(len(lost_now) == CLOUD_FRAMES, f"{len(lost_now)} StarLost events for {CLOUD_FRAMES} clouded frames")
    for e in lost_now:
        code = e.get("ErrorCode")
        expect(code in NO_STAR_ERRORS and e.get("Status") == NO_STAR_ERRORS[code],
               f"a clouded frame is not a frame with no star found ({NO_STAR_ERRORS}): {strip(e)}")
    expect(states <= {"Guiding", "LostLock"}, f"app states while clouded: {sorted(states)}")
    expect(cli.call("get_app_state") == "Guiding", "not guiding after the clouds")
    # the first frame after the clouds is a GuideStep: the star was reacquired at once
    frames = [e.get("Frame") for e in lost_now]
    expect(resumed[0].get("Frame") == frames[-1] + 1,
           f"first GuideStep after the clouds is frame {resumed[0].get('Frame')}, the last StarLost {frames[-1]}")
    statuses = ", ".join(f"{e.get('Frame')} {e.get('Status')!r}" for e in lost_now)
    return (f"{len(lost_now)} StarLost ({statuses}), states seen {sorted(states)}; GuideSteps again from frame "
            f"{resumed[0].get('Frame')}")


def run_smoke(args, fake, work):
    cmd = [sys.executable, os.path.join(REPO, "scripts", "phd2_rpc_smoke.py"), "--port", str(args.rpc_port),
           "--timeout", "30", "--calibration-timeout", str(args.calibration_timeout),
           "--fake-alpaca-url", fake.url]
    print(f"e2e: running {' '.join(cmd)}", flush=True)
    with open(os.path.join(work, "smoke.out"), "wb") as out:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=args.smoke_timeout,
                              check=False)
        out.write(proc.stdout)
    text = proc.stdout.decode("utf-8", errors="replace")
    summary = [line for line in text.splitlines() if line.startswith("SMOKE RESULT") or line.startswith("FAIL")]
    expect(proc.returncode == 0, "smoke failed:\n" + "\n".join(summary))
    return summary[-1] if summary else "ok"


def setup_profile(cli, fake):
    """A new profile on the fake camera and mount, connected. Returns the profile to go back to."""
    base = cli.call("get_profile")
    name = f"e2e_{int(time.time())}"
    cli.call("create_profile", {"name": name, "select": True})
    cli.call("set_alpaca_server", {"host": fake.host, "port": fake.port, "camera_device": 0, "telescope_device": 0})
    cli.call("set_selected_camera", {"camera": f"Alpaca Camera [{fake.host}:{fake.port}/0]"})
    cli.call("set_selected_mount", {"mount": f"Alpaca Mount [{fake.host}:{fake.port}/0]"})
    cli.call("set_exposure", {"exposure": 500})
    cli.call("set_connected", {"connected": True}, timeout=60)
    expect(cli.call("get_connected") is True, "the equipment did not connect")
    return base, name


def teardown_profile(cli, base, name):
    cli.call("stop_capture")
    cli.wait_state("Stopped", 30)
    cli.call("set_connected", {"connected": False}, timeout=60)
    cli.call("set_profile", {"id": base["id"]})
    cli.call("delete_profile", {"name": name, "delete_dark_files": True})
    return f"disconnected; profile {base.get('name')} again, {name} deleted"


def collect_logs(daemon, work):
    """Copy the daemon's debug and guide logs into the work dir root, where CI uploads them from."""
    src = os.path.join(daemon.home, "PHD2")
    if os.path.isdir(src):
        for f in os.listdir(src):
            shutil.copy(os.path.join(src, f), os.path.join(work, f))


def lane_steps(lane, step, cli, fake, args, ctx):
    """The scenarios of a lane, in order; a scenario that needs the one before it runs only after it passed."""

    def connect():
        ctx["base"], ctx["name"] = setup_profile(cli, fake)
        return f"profile {ctx['name']}"

    if lane == "main":
        step("HTTP data port before the first frame (N2)", lambda: scenario_http_before_first_frame(args))
        if not args.skip_smoke:
            step("smoke (phd2_rpc_smoke.py)", lambda: run_smoke(args, fake, ctx["work"]))
        if step("connect the fake camera and mount", connect):
            step("noise reduction is the one in effect when the frame was scheduled (#210)",
                 lambda: scenario_noise_reduction_at_schedule(cli))
            step("HTTP data port: /api/frame.jpg and /api/capture (N2)",
                 lambda: scenario_http_frame_and_capture(cli, args, ctx["work"]))
            if step("calibrate and guide (event order)", lambda: scenario_calibrate_and_guide(cli, args)):
                step("guide history matches the GuideSteps (#133)", lambda: scenario_guide_history(cli))
                if step("recalibrate on a new target", lambda: scenario_new_target(cli, fake, args)):
                    step("frame drops while guiding (#136)", lambda: scenario_frame_drops(cli, fake, args))
        return
    # physics: one calibration, then the guiding scenarios on it
    if step("connect the fake camera and mount", connect) and step(
            f"calibration truth (camera {PHYSICS_CAMERA_ANGLE_DEG:g} deg, Dec {PHYSICS_DEC:g}, backlash)",
            lambda: scenario_calibration_truth(cli, fake, args)):
        step(f"dither {DITHER_AMOUNT} px and settle", lambda: scenario_dither_and_settle(cli, fake, args))
        step(f"clouds for {CLOUD_FRAMES} frames while guiding", lambda: scenario_clouds(cli, fake, args))


def run_lane(lane, args, work, t0):
    """One daemon and one fake, the lane's scenarios, then the shutdown. Returns (passed, failures)."""
    failures, results = [], []
    fake = daemon = cli = None
    ctx = {"work": work}

    def step(name, fn):
        t = time.time()
        try:
            result = fn()
            results.append(name)
            print(f"PASS {name} ({time.time() - t:.0f} s): {result}", flush=True)
            return True
        except Failure as e:
            failures.append((name, str(e)))
            print(f"FAIL {name} ({time.time() - t:.0f} s): {e}", flush=True)
            return False
        except RunTimeout:  # ends the run, not just this step: the alarm only fires once
            raise
        except Exception as e:  # a bug in a check is a failed step, and the daemon is still shut down after it
            failures.append((name, repr(e)))
            print(f"FAIL {name} ({time.time() - t:.0f} s): {e!r}", flush=True)
            traceback.print_exc()
            return False

    try:
        fake = Fake(args.fake_port, work, PHYSICS_FAKE_ARGS if lane == "physics" else ())
        daemon = Daemon(args, work)
        cli = Client(args.rpc_port, daemon, os.path.join(work, "events.jsonl"), connect_timeout=120)
        print(f"e2e: daemon up on :{args.rpc_port} after {time.time() - t0:.0f} s", flush=True)
        lane_steps(lane, step, cli, fake, args, ctx)
    except (Failure, RunTimeout) as e:
        failures.append(("run", str(e)))
        print(f"FAIL run: {e}", flush=True)
    finally:
        signal.alarm(0)
        # a SIGTERM from here on must not cut the teardown short: it would leave the daemon running. It is recorded
        # instead, and the handler it replaced is put back once the teardown is done
        prev_term = signal.signal(signal.SIGTERM, lambda signum, frame: TERMINATED.append(signum))
        try:
            if cli is not None and daemon is not None and not daemon.exited():
                # leave the profile as it was, then shut down over the API
                if "name" in ctx:
                    step("disconnect and restore the profile", lambda: teardown_profile(cli, ctx["base"], ctx["name"]))
                step("shutdown", lambda: shutdown(cli, daemon))
        finally:
            if cli is not None:
                cli.close()
            if daemon is not None:
                daemon.kill()
                status = daemon.proc.returncode
                if status not in (0, None) and not any(n == "shutdown" for n, _ in failures):
                    failures.append(("daemon exit", f"the daemon exited with status {status}"))
                reports = daemon.sanitizer_reports()
                if reports:
                    failures.append(("sanitizers", "\n".join(reports[:20])))
                collect_logs(daemon, work)
            if fake is not None:
                fake.stop()
            signal.signal(signal.SIGTERM, prev_term)
    return results, failures


AUTO_DISPLAY_LANE_OFFSET = 100

# Each SIGTERM this run received: on_term records it and ends the run; during a lane's teardown it is only recorded,
# so the teardown finishes, and the physics lane is then stopped at once instead of waited for
TERMINATED = []


# The lanes run beside the main lane by --lane all, each a child process on the main lane's instance, fake port and
# display + its offset: the physics lane (a second copy of this script) and the gear and faults lanes of
# scripts/e2e_equipment_scenarios.py
CHILD_LANES = (("physics", 1), ("gear", 2), ("faults", 3))


class LaneChild:
    """A lane run beside the main lane as a child process: its own daemon instance (--instance + offset), fake port
    (+ offset), X display (+ offset) and work dir (<work-dir>/<lane>). Its output is printed with a [<lane>]
    prefix as it comes."""

    def __init__(self, args, work, lane, offset):
        self.lane = lane
        self.work = os.path.join(work, lane)
        display = args.display if args.display == "auto" else str(int(args.display) + offset)
        # With --display auto the lanes start xvfb-run -a at about the same moment, and two of them can take the
        # same free display (it checks for a lock file, then starts Xvfb): each lane's search starts
        # AUTO_DISPLAY_LANE_OFFSET x its offset higher, so they never meet.
        auto_from = args.auto_display_from + AUTO_DISPLAY_LANE_OFFSET * offset
        # its own whole-run timeout ends it before this one's, so it shuts its daemon down itself
        common = ["--phd2", args.phd2, "--instance", str(args.instance + offset), "--fake-port",
                  str(args.fake_port + offset), "--display", display, "--auto-display-from", str(auto_from),
                  "--work-dir", self.work, "--timeout", str(max(60.0, args.timeout - 120.0))]
        if lane == "physics":
            cmd = [sys.executable, os.path.abspath(__file__), "--lane", "physics", *common,
                   "--calibration-timeout", str(args.calibration_timeout * PHYSICS_CALIBRATION_TIMEOUT_FACTOR),
                   "--guide-steps", str(args.guide_steps)]
        else:
            cmd = [sys.executable, os.path.join(REPO, "scripts", "e2e_equipment_scenarios.py"), "--lane", lane,
                   *common, "--calibration-timeout", str(args.calibration_timeout)]
        print(f"e2e: starting the {lane} lane: {' '.join(cmd)}", flush=True)
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.reader = threading.Thread(target=self._relay, daemon=True)
        self.reader.start()

    def _relay(self):
        for line in self.proc.stdout:
            print(f"[{self.lane}] " + line.decode("utf-8", errors="replace").rstrip("\n"), flush=True)

    def finish(self, timeout):
        """Wait for the lane, then SIGTERM it (and SIGKILL it if that does not end it) when the timeout passes or
        this run is terminated while it waits. Returns its failures."""
        # A SIGTERM here is only recorded: raised out of a wait it would leave the lane running
        prev_term = signal.signal(signal.SIGTERM, lambda signum, frame: TERMINATED.append(signum))
        try:
            deadline = time.time() + max(1.0, timeout)
            while self.proc.poll() is None and not TERMINATED and time.time() < deadline:
                try:
                    self.proc.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    pass
            if self.proc.poll() is None:
                self.proc.terminate()  # its SIGTERM handler shuts its daemon and fake down
                try:
                    self.proc.wait(timeout=240)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait()
        finally:
            signal.signal(signal.SIGTERM, prev_term)
        self.reader.join(timeout=10)
        if self.proc.returncode != 0:
            return [(f"{self.lane} lane", f"exited with status {self.proc.returncode}; logs in {self.work}")]
        return []

    def ok(self):
        return self.proc.returncode == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--phd2", required=True, help="the phd2.bin to run (an ASan/UBSan build in CI)")
    ap.add_argument("--instance", type=int, default=10, help="daemon instance (-i); JSON-RPC on 4400 + N - 1")
    ap.add_argument("--fake-port", type=int, default=11150, help="the fake Alpaca server's port")
    ap.add_argument("--display", default="110", help="X display number for xvfb-run, or auto (xvfb-run -a)")
    ap.add_argument("--auto-display-from", type=int, default=99,
                    help="with --display auto, the first display number xvfb-run -a tries (its own default, 99); "
                         f"the physics lane of --lane all searches from {AUTO_DISPLAY_LANE_OFFSET} higher")
    ap.add_argument("--work-dir", required=True, help="the scratch HOME and every log go here (emptied first)")
    ap.add_argument("--timeout", type=float, default=2400, help="the whole run, seconds")
    ap.add_argument("--smoke-timeout", type=float, default=1200)
    ap.add_argument("--calibration-timeout", type=float, default=600,
                    help="seconds to CalibrationComplete, here and in the smoke script's #137 check (the physics "
                         f"lane gets {PHYSICS_CALIBRATION_TIMEOUT_FACTOR:g}x); expect a calibration to take about "
                         "270 s under ASan on the ubuntu-24.04-arm runner, the physics lane's about 1.4x that")
    ap.add_argument("--guide-steps", type=int, default=10, help="GuideSteps to see after each SettleDone")
    ap.add_argument("--skip-smoke", action="store_true")
    ap.add_argument("--lane", choices=("all", "main", "physics"), default="all",
                    help="main: the smoke script and the scenarios on the plain fake; physics: the calibration "
                         "truth, dither and clouds scenarios on a fake with camera angle, cos(Dec), backlash, "
                         "drift, PE and seeing; all (the default): the main lane and, at the same time, the "
                         "--child-lanes on instance, fake port and display + 1 (physics), + 2 (gear) and + 3 "
                         "(faults); gear and faults are scripts/e2e_equipment_scenarios.py's")
    ap.add_argument("--child-lanes", default=",".join(name for name, _ in CHILD_LANES),
                    help="with --lane all, the lanes run beside the main lane (comma-separated; empty for none)")
    args = ap.parse_args()
    args.rpc_port = 4400 + args.instance - 1
    args.http_port = 8080 + args.instance - 1

    work = os.path.abspath(args.work_dir)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    t0 = time.time()

    # the whole-run timeout: a hung daemon or check must end the run, not the CI job's own timeout. SIGTERM (the
    # main lane stopping a physics lane that overran) ends the run the same way, so the daemon is still shut down.
    def on_alarm(signum, frame):
        raise RunTimeout(f"the run took longer than {args.timeout:.0f} s")

    def on_term(signum, frame):
        TERMINATED.append(signum)
        raise RunTimeout("terminated")

    signal.signal(signal.SIGALRM, on_alarm)
    signal.signal(signal.SIGTERM, on_term)
    signal.alarm(int(args.timeout))

    wanted = [name for name in args.child_lanes.split(",") if name]
    unknown = sorted(set(wanted) - {name for name, _ in CHILD_LANES})
    if unknown:
        ap.error(f"unknown --child-lanes {unknown}")
    children = [LaneChild(args, work, name, offset) for name, offset in CHILD_LANES
                if args.lane == "all" and name in wanted]
    results, failures = [], []
    try:
        results, failures = run_lane("physics" if args.lane == "physics" else "main", args, work, t0)
    finally:
        for child in children:
            failures += child.finish(args.timeout - (time.time() - t0))
            if child.ok():
                results.append(f"{child.lane} lane")
        if TERMINATED and not any(name == "run" for name, _ in failures):
            failures.append(("run", "terminated"))  # during a teardown or a child lane's wait

    print("")
    print(f"e2e: {len(results)} passed, {len(failures)} failed, {time.time() - t0:.0f} s; logs in {work}")
    for name, err in failures:
        print(f"- {name}: {err}")
    return 1 if failures else 0


def shutdown(cli, daemon):
    cli.call("shutdown")
    status = daemon.wait(60)
    expect(status is not None, "the daemon did not exit within 60 s of shutdown")
    expect(status == 0, f"the daemon exited with status {status} after shutdown")
    return "exit status 0"


if __name__ == "__main__":
    sys.exit(main())
