#!/usr/bin/env python3
"""Daemon end-to-end run against the fake Alpaca server (#142 Tier 1).

Starts scripts/fake_alpaca_camera.py and the guider daemon (phd2.bin, headless under xvfb-run with a scratch HOME),
then drives the daemon over JSON-RPC the way Ara does:

  smoke          scripts/phd2_rpc_smoke.py with --fake-alpaca-url
  calibrate      connect the Alpaca camera and mount, loop, find_star, guide with recalibrate true: a full
                 calibration, StartGuiding, a settle that ends in exactly one SettleDone with Status 0, then
                 guiding; the event order Ara consumes is checked on the way (design/API_CONTRACT.md)
  new-target     stop, slew the fake mount to another target, guide with recalibrate true: a second full
                 calibration, made at the new Dec
  frame-drop     while guiding, the camera fails 1 frame, then 2 in a row, with Alpaca 1280: each one is a
                 StarLost and guiding goes on (#136)

then shuts the daemon down over RPC. It fails on a failed check, on the daemon exiting before the shutdown or with a
non-zero status, and on any AddressSanitizer / UndefinedBehaviorSanitizer report in the daemon's output. Everything
it collects (the daemon's stdout/stderr and debug log, the fake's log, the smoke output, every event received) is
left in --work-dir.

    python3 scripts/e2e_guide_scenarios.py --phd2 build-asan/phd2.bin --work-dir /tmp/e2e
"""
import argparse
import json
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
        cmd += ["-a"] if args.display == "auto" else ["-n", args.display]
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

    def __init__(self, port, work):
        self.host, self.port = "127.0.0.1", port
        self.url = f"http://{self.host}:{port}"
        self.log = open(os.path.join(work, "fake_alpaca.log"), "wb")
        cmd = [sys.executable, os.path.join(REPO, "scripts", "fake_alpaca_camera.py"), "--host", self.host,
               "--port", str(port)]
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


def scenario_calibrate_and_guide(cli, args):
    start = cli.mark()
    expect(cli.call("loop") == 0, "loop did not return 0")
    cli.wait_events(start, "LoopingExposures", count=3, timeout=60, abort_on=("LoopingExposuresStopped",))
    pos = cli.call("find_star")
    expect(isinstance(pos, list) and len(pos) == 2, f"find_star returned {pos}")
    cli.wait_events(start, "StarSelected", timeout=30)

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
    total = sum(1 for e in cli.events_since(0) if e["Event"] == "StartCalibration")
    expect(total >= 2, f"{total} StartCalibration events since connect, expected a second calibration")
    return (f"second calibration at Dec {after.get('declination')} (was {before.get('declination')}), "
            f"steps {legs}, settled in {done.get('TotalFrames')} frames")


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


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--phd2", required=True, help="the phd2.bin to run (an ASan/UBSan build in CI)")
    ap.add_argument("--instance", type=int, default=10, help="daemon instance (-i); JSON-RPC on 4400 + N - 1")
    ap.add_argument("--fake-port", type=int, default=11150, help="the fake Alpaca server's port")
    ap.add_argument("--display", default="110", help="X display number for xvfb-run, or auto (xvfb-run -a)")
    ap.add_argument("--work-dir", required=True, help="the scratch HOME and every log go here (emptied first)")
    ap.add_argument("--timeout", type=float, default=2400, help="the whole run, seconds")
    ap.add_argument("--smoke-timeout", type=float, default=1200)
    ap.add_argument("--calibration-timeout", type=float, default=600,
                    help="seconds to CalibrationComplete, here and in the smoke script's #137 check; a calibration "
                         "took about 270 s under ASan on the ubuntu-24.04-arm runner")
    ap.add_argument("--guide-steps", type=int, default=10, help="GuideSteps to see after each SettleDone")
    ap.add_argument("--skip-smoke", action="store_true")
    args = ap.parse_args()
    args.rpc_port = 4400 + args.instance - 1

    work = os.path.abspath(args.work_dir)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    t0 = time.time()

    # the whole-run timeout: a hung daemon or check must end the run, not the CI job's own timeout
    def on_alarm(signum, frame):
        raise RunTimeout(f"the run took longer than {args.timeout:.0f} s")

    signal.signal(signal.SIGALRM, on_alarm)
    signal.alarm(int(args.timeout))

    failures, results = [], []
    fake = daemon = cli = None
    ctx = {}

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
        fake = Fake(args.fake_port, work)
        daemon = Daemon(args, work)
        cli = Client(args.rpc_port, daemon, os.path.join(work, "events.jsonl"), connect_timeout=120)
        print(f"e2e: daemon up on :{args.rpc_port} after {time.time() - t0:.0f} s", flush=True)

        if not args.skip_smoke:
            step("smoke (phd2_rpc_smoke.py)", lambda: run_smoke(args, fake, work))

        def connect():
            ctx["base"], ctx["name"] = setup_profile(cli, fake)
            return f"profile {ctx['name']}"

        if step("connect the fake camera and mount", connect) and step(
                "calibrate and guide (event order)", lambda: scenario_calibrate_and_guide(cli, args)):
            if step("recalibrate on a new target", lambda: scenario_new_target(cli, fake, args)):
                step("frame drops while guiding (#136)", lambda: scenario_frame_drops(cli, fake, args))
    except (Failure, RunTimeout) as e:
        failures.append(("run", str(e)))
        print(f"FAIL run: {e}", flush=True)
    finally:
        signal.alarm(0)
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
