#!/usr/bin/env python3
"""Equipment and fault scenarios of the daemon end-to-end run (#142 Tier 1).

Characterization: each scenario pins what the daemon does today, at the JSON-RPC and event level Ara sees, as the
safety net for #115 step 3 (connect, disconnect, auto-connect, the #104 connect retry and the #105 camera reconnect
moving out of GearDialog). A check that fails is a change of behaviour, not necessarily a bug; the bugs found while
writing these are pinned as they are and reported, not fixed here.

Two lanes, run by scripts/e2e_guide_scenarios.py beside its main and physics lanes (each on its own instance, fake
port and display; see that script's LaneChild), or alone with --lane:

the gear lane, on a fake that answers ImageBytes (no calibration but the camera-change and binning scenarios'):

  connect side effects    set_connected per device and for all: the result, the events, get_connected and
                          get_current_equipment; the #139 latch: a profile pixel size off the camera's is acted on
                          once (one Alert, the calibration step and both MinMoves reset to the defaults for the new
                          image scale, the driver's pixel size saved), and the reconnects after it, all and per
                          device, keep what the client set since; the refusals while equipment is connected
                          (set_selected_camera / _mount, set_alpaca_server, set_profile_setup, set_profile,
                          delete_profile; set_profile to the current profile is a no-op); disconnects while looping
  camera change           #139: calibrate, change the pixel size, connect all with the mount refusing to connect:
  with the mount off      the camera connects, the mount fails with the server's reason; the mount connected later
                          by a per-device set_connected clears the calibration then (EquipmentReconnected mount),
                          and the saved calibration is not auto-loaded again in this process
  guide speed sources     #115 N4, before S4.1 moves MakeImageScaleAdjustments and DetermineGuideSpeed out of
                          AdvancedDialog: the #139 latch's image scale change on connect sets the calibration step
                          for the guide speed of each branch: the mount's rates (the lower of RA and Dec), the saved
                          calibration's when the mount's read fails or its rates are invalid, 0.5x with the mount
                          not connected or nothing saved; with no mount selected nothing is adjusted, the Alert goes
                          out
  binning change on       #115 N4: a calibration at bin 1, software binning 2, restore_calibration, guide with no
  guide start             recalibration: AdjustCalibrationForScopePointing halves the rates, the scale Alert, the
                          step and MinMoves reset for bin 2 and the calibration cleared; guiding goes on uncalibrated
                          with no calibration (pinned as it is), and the next guide calibrates
  auto-connect            the daemon restarted with --auto-connect (as debian/openastro-guider.service runs it)
                          against the running fake: connected by the time the RPC port answers, the saved
                          calibration auto-loaded, no EquipmentReconnected
  auto-connect, late      restarted with --auto-connect and no fake: not connected; the #104 retry 15 s and then
  fake (#104)             30 s after each failure, logged, with no event and no Alert; the fake started after the
                          second failure: EquipmentReconnected camera and mount at the third attempt, connected
  stale selections        the profile file edited offline, then a restart: a camera and a mount in no list
                          resolve to the first camera (the Alpaca one) and to '' (no mount); a camera and a mount
                          differing from the lists only in case resolve to the lists' items, which connect
  guide connects first    restarted with nothing connected: guide connects the camera and the mount (#104), then
                          calibrates and guides; after set_connected false it refuses, naming both devices as
                          disconnected by the user
  set_alpaca_server       with nothing connected, the mount wanted by the #104 retry and the camera disconnected by
  carry                   the user: the Alpaca selections follow the new server, the mount is tried there at once
                          and the camera is not; back on the fake, the mount reconnects at once
  shutdown during a       the fake holding the mount's Connected=true PUT while the #104 probe waits on it: shutdown
  #104 probe              exits 0, within the held request, with the retry stopped and no connect after it
  server change (#236)    set_alpaca_server to a second fake, nothing connected: the imaging-gear reader reads the
                          new server at once, with no other call to wake it, and get_imaging_gear never reports the
                          server it left (unavailable until the new one is read)

the faults lane, on a fake that answers ImageBytes, with one calibration (a long calibration step, so it is quick):

  camera drop mid-loop    one Alpaca 1280 while looping stops the loop (one LoopingExposuresStopped, #186); a 1031
                          reconnects the camera (EquipmentDisconnected reconnecting, EquipmentReconnected) and the
                          loop goes on
  mount read errors       declination and sideofpier failing through the whole calibration: the calibration
  during calibration      completes, get_calibration_data reporting the unknown Dec and pierSide as null (#189);
                          the profile's first calibration turns auto_restore_calibration on
  pulseguide errors       pulseguide failing while guiding: the first failed pulse raises an Alert, and then at most
                          one every 30 s (#188), not one per failed pulse; guiding goes on through fewer than 10
                          failed moves in a row, the mount stays connected; failing on, the 10th failed move in a row
                          stops guiding (#213): an Alert naming the mount, then GuidingStopped; guide (no
                          recalibration) settles once the pulses go through again
  guide output off        set_guide_output_enabled false while guiding: the refused moves are not failed moves
                          (#213), so guiding goes on past 10 frames with no failed-moves Alert
  3rd camera drop         3 frames in a row failing with 1280 while guiding: 2 StarLost, then the #103 reconnect
                          (EquipmentDisconnected reconnecting, EquipmentReconnected) and guiding goes on
  capture timeout         an exposure that never completes (/fake/hold): the camera watchdog disconnects and
                          reconnects the camera, and guiding goes on
  fake killed between     the Alpaca server gone mid-guide between two camera calls (the fake stops listening while
  calls (#105, #190)      it holds an imageready, so no call is cut, #211): the next call is refused and reconnects
                          the camera (#190), the reconnect fails, guiding stops, the camera reads disconnected and the
                          mount connected; back on the same port, the #104 retry reconnects the camera and guide (no
                          recalibration) settles
  fake killed mid-request the fake killed while a startexposure waits on it (#211): that call gets no response, which
  (#211, #237)            is transient like a timeout (#237, #227): one StarLost with the reason, guiding goes on; the
                          next call is refused and takes the #190 reconnect, which fails, guiding stops, the camera
                          reads disconnected and the mount connected; back on the same port, the #104 retry
                          reconnects the camera and guide (no recalibration) settles
  Guiding Assistant       start while guiding: guide output, multi-star and variable delay off during the run;
                          guiding on past 10 frames with output off (#213); stopped after a few samples: restored,
                          guiding going on with the same star and lock position and the secondary stars found again
                          (#187); apply's refusals, and min-move applied with a valid JSON response (#184); a run
                          started from looping guides and ends looping
  reconnect throttle      a 4th camera reconnect within 60 s: EquipmentReconnectFailed, then the #104 retry
  (#105 -> #104)          reconnects it (EquipmentReconnected) 15 s later, capture not restarted
  GA exit restore         shutdown during a run: the multi-star and variable-delay settings the run turned off read
                          as before it after a restart

Once a faults lane step has failed, each scenario after the calibration first gets back to where it starts on a
passing lane (the fake up, the equipment connected, guiding on the kept calibration, or stopped; #233), and is
skipped with the reason if that fails, or if the calibration did, so one failure is reported once.

A lane fails on a failed check, a daemon exit before its shutdown or with a non-zero status, and any sanitizer
report. Everything it collects (each daemon's output and debug log, each fake's log, every event) is left in its
--work-dir.

    python3 scripts/e2e_equipment_scenarios.py --lane gear --phd2 build-asan/phd2.bin --instance 12 \\
        --fake-port 11152 --display 112 --work-dir /tmp/e2e/gear
"""
import argparse
import glob
import json
import math
import os
import re
import shutil
import signal
import socket
import sys
import time
import traceback

import e2e_guide_scenarios as e2e
import e2e_persistence as persist

expect = e2e.expect
Failure = e2e.Failure
strip = e2e.strip

# ImageBytes frames keep the fake's CPU low while the other lanes run beside it
FAKE_ARGS = ("--imagebytes",)
FAKE_PIXEL_UM = 3.75  # the fake camera's pixelsizex
FOCAL_MM = 400
# A pixel size off the fake camera's by more than 1 % (#139's 2.4 against 3.75)
WRONG_PIXEL_UM = 2.4
# A calibration of a few steps a leg: the fake moves the star 1.94 px/s at 1x sidereal, so a 2500 ms step is 4.9 px
# and 15 px takes 4 steps
QUICK_CALIBRATION = {"calibration_duration": 2500, "calibration_distance": 15}
EXPOSURE_MS = 500
SCALE_ALERT = "Binning or camera pixel size changed unexpectedly"
PULSEGUIDE_ALERT = "PulseGuide command to mount has failed"
PULSEGUIDE_ALERT_THROTTLE_S = 30  # the shortest time between two PulseGuide failed Alerts (#188)
# the failed guide moves in a row of a mount that stop guiding (guide_move_plan::MAX_FAILED_GUIDE_MOVES, #213), and
# the Alert that says so
MAX_FAILED_GUIDE_MOVES = 10
FAILED_MOVES_ALERT = "pulses keep failing"
# a failed move in the debug log: the worker's line after each Mount::MoveOffset (MOVE_ERROR is 1)
FAILED_MOVE_LINE = r"move complete, result=1\b"
# MyFrame::OnMoveComplete's line when the failed moves stop guiding
STOP_LINE = r"(\d+) guide moves in a row failed, stopping guiding"
# what a reconnect window is: MyFrame::DoTryReconnect allows 3 camera reconnects in 60 s (wall clock)
RECONNECT_WINDOW_S = 60
RECONNECT_MAX = 3
# the #104 schedule (src/connect_retry.h)
RETRY_FIRST_S, RETRY_SECOND_S = 15, 30
# a GA run's shortest duration (guiding_assistant_start), never reached: each run here is stopped
GA_DURATION_S = 60

GUIDE_FAILURES = e2e.GUIDE_FAILURES
# the abort list of a scenario whose camera is to be disconnected and reconnected while guiding
GUIDING_ENDED = ("CalibrationFailed", "GuidingStopped", "LoopingExposuresStopped", "EquipmentReconnectFailed")


def camera_choice(fake):
    return f"Alpaca Camera [{fake.host}:{fake.port}/0]"


def mount_choice(fake):
    return f"Alpaca Mount [{fake.host}:{fake.port}/0]"


# get_current_equipment's names once the drivers connected: the choice, then the device's Alpaca name (and the
# camera's description), which the drivers append on connect and keep after a disconnect
def camera_connected_name(fake):
    return camera_choice(fake) + " - FakeCam (Synthetic Alpaca camera for tests)"


def mount_connected_name(fake):
    return mount_choice(fake) + " - FakeScope"


def call_error(cli, method, params=None, timeout=30):
    """The error of a call that must fail: (code, message)."""
    msg = cli.request(method, params, timeout)
    err = msg.get("error")
    expect(err, f"{method} {params} succeeded ({msg.get('result')!r}), expected an error")
    return err.get("code"), err.get("message")


def expect_error(cli, method, params, code, text):
    got = call_error(cli, method, params)
    expect(got == (code, text), f"{method} {params} failed with {got}, expected ({code}, {text!r})")
    return text


def settle_events(cli, start, quiet_s=1.0, timeout=10.0):
    """The events since `start`, once none has arrived for quiet_s (deferred ones, posted with CallAfter, included)."""
    deadline = time.time() + timeout
    n, last = -1, time.time()
    while time.time() < deadline:
        evs = cli.events_since(start)
        if len(evs) != n:
            n, last = len(evs), time.time()
        elif time.time() - last >= quiet_s:
            break
        time.sleep(0.1)
    return cli.events_since(start)


def names_of(evs, skip=("LoopingExposures", "GuideStep", "Settling", "StarSelected", "GuidingAssistantProgress")):
    return [e["Event"] for e in evs if e["Event"] not in skip]


def alerts(evs, text=None):
    return [e for e in evs if e["Event"] == "Alert" and (text is None or text in str(e.get("Msg", "")))]


def equipment(cli):
    """get_current_equipment as {device: (name, connected)}."""
    return {k: (v.get("name"), v.get("connected")) for k, v in cli.call("get_current_equipment").items()}


def connected_by_device(cli):
    return {d: cli.call("get_connected", {"device": d}) for d in ("all", "camera", "mount", "rotator")}


def min_moves(cli):
    return (cli.call("get_algo_param", {"axis": "ra", "name": "minMove"}),
            cli.call("get_algo_param", {"axis": "dec", "name": "minMove"}))


def smart_default_min_move(pixel_um, focal_mm=FOCAL_MM, binning=1):
    """GuideAlgorithm::SmartDefaultMinMove (src/guide_algorithm.cpp): the MinMove an image scale change resets both
    axes to (MyFrame::MakeImageScaleAdjustments); 0.2316 px for the fake's 3.75 um at 400 mm."""
    scale = 206.265 * pixel_um * binning / focal_mm
    return max(0.1515 + 0.1548 / scale, 0.15)


def setup(cli):
    return cli.call("get_profile_setup")


def raw_call(port, method, params, timeout=30):
    """One JSON-RPC call on a connection of its own, the response line returned as the daemon wrote it: for a
    response that is not valid JSON, which e2e.Client's reader cannot take."""
    req_id = 9001
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall((json.dumps({"jsonrpc": "2.0", "method": method, "params": params, "id": req_id}) + "\n").encode())
        for line in sock.makefile("rb"):
            text = line.decode("utf-8", errors="replace").strip()
            if f'"id":{req_id}' in text.replace(" ", "") and '"Event"' not in text:
                return text
    raise Failure(f"no response to {method}")


def fake_log_size(lane):
    return os.path.getsize(lane.fake.log.name)


def fake_log_since(lane, offset):
    lane.fake.log.flush()
    with open(lane.fake.log.name, "rb") as f:
        f.seek(offset)
        return f.read().decode("utf-8", errors="replace")


def wait_fake_stall(lane, at, call, ms, timeout):
    """Wait for a camera request to the stalled endpoint to reach the fake (its stall log line)."""
    deadline = time.time() + timeout
    while f"stalling camera {ms} ms: /api/v1/camera/0/{call}" not in fake_log_since(lane, at):
        expect(time.time() < deadline, f"no {call} reached the fake within {timeout} s")
        time.sleep(0.1)


def kill_fake_during_startexposure(lane, timeout=30):
    """Stop the fake while the guider waits on a startexposure, the kill a loaded machine can give (#211)."""
    at = fake_log_size(lane)
    lane.fake.request("PUT", "/fake/stall", {"device": "camera", "ms": 60000, "call": "startexposure"})
    try:
        wait_fake_stall(lane, at, "startexposure", 60000, timeout)
    finally:
        lane.stop_fake()  # also on a timeout, so the stall goes with the fake and the restart after it starts clean


def refuse_fake_between_calls(lane, timeout=30):
    """Make the fake refuse every connection from the guider's next call on, with none of its calls cut (#211).

    An imageready is held on the fake while its listening socket is closed (/fake/stop_listening): that call still
    gets its answer, and the call after it is refused, as when the server goes between two calls. The process is
    left running, not listening; the caller stops it once the guider has seen the refusal."""
    at = fake_log_size(lane)
    lane.fake.request("PUT", "/fake/stall", {"device": "camera", "ms": 2000, "call": "imageready"})
    try:
        wait_fake_stall(lane, at, "imageready", 2000, timeout)
        lane.fake.request("PUT", "/fake/stop_listening")
    except BaseException:
        lane.stop_fake()  # gone, not just stalled: the lane starts it again after a failed step
        raise


class DebugLog:
    """The daemon's debug log (PHD2_DebugLog_*.txt under its HOME), read from a mark on."""

    def __init__(self, home):
        self.home = home
        self.marks = {}

    def _files(self):
        return sorted(glob.glob(os.path.join(self.home, "**", "PHD2_DebugLog_*.txt"), recursive=True))

    def mark(self):
        self.marks = {f: os.path.getsize(f) for f in self._files()}

    def text(self):
        out = []
        for f in self._files():
            with open(f, "rb") as fh:
                fh.seek(self.marks.get(f, 0))
                out.append(fh.read().decode("utf-8", errors="replace"))
        return "".join(out)

    def wait(self, pattern, timeout, daemon=None, count=1):
        """Wait for `count` lines matching `pattern` since the mark; returns the matches."""
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        while True:
            found = rx.findall(self.text())
            if len(found) >= count:
                return found
            if daemon is not None:
                daemon.check_alive()
            expect(time.time() < deadline, f"{len(found)} of {count} debug log lines /{pattern}/ within {timeout:.0f} s")
            time.sleep(0.25)


class Lane:
    """One lane: a fake, a daemon (started again where a scenario needs it), a JSON-RPC client."""

    def __init__(self, name, args, work):
        self.name, self.args, self.work = name, args, work
        self.fake = self.daemon = self.cli = None
        self.fakes = 0
        self.daemons = 0
        self.profile = None  # (base profile, the lane's profile name), to restore at the end
        self.results, self.failures, self.skipped = [], [], []

    # --- processes

    def start_fake(self):
        self.fakes += 1
        log = "fake_alpaca.log" if self.fakes == 1 else f"fake_alpaca-{self.fakes}.log"
        self.fake = e2e.Fake(self.args.fake_port, self.work, FAKE_ARGS, log_name=log)
        return self.fake

    def stop_fake(self):
        if self.fake is not None:
            self.fake.stop()

    def start_daemon(self, extra_args=()):
        self.daemons += 1
        n = self.daemons
        # marked before the start: a daemon started again may append to the debug log the last one wrote
        self.debug = DebugLog(os.path.join(self.work, "home"))
        self.debug.mark()
        self.daemon = e2e.Daemon(self.args, self.work, log_name="phd2.out" if n == 1 else f"phd2-{n}.out",
                                 extra_args=extra_args)
        self.cli = e2e.Client(self.args.rpc_port, self.daemon, os.path.join(self.work, f"events-{n}.jsonl"
                                                                            if n > 1 else "events.jsonl"),
                              connect_timeout=180)
        return self.cli

    def end_daemon(self):
        """Shut the daemon down over RPC, check that it exited 0, and free its display for the next start."""
        cli, daemon = self.cli, self.daemon
        try:
            return e2e.shutdown(cli, daemon)
        finally:
            cli.close()
            daemon.kill()
            persist.release_display(daemon)
            self.check_sanitizers(daemon)
            self.cli = None

    def check_sanitizers(self, daemon):
        reports = daemon.sanitizer_reports()
        if reports:
            self.failures.append((f"sanitizers ({os.path.basename(daemon.log_path)})", "\n".join(reports[:20])))

    # --- steps

    def step(self, name, fn):
        t = time.time()
        try:
            result = fn()
            self.results.append(name)
            print(f"PASS {name} ({time.time() - t:.0f} s): {result}", flush=True)
            return True
        except Failure as e:
            self.failures.append((name, str(e)))
            print(f"FAIL {name} ({time.time() - t:.0f} s): {e}", flush=True)
            return False
        except e2e.RunTimeout:
            raise
        except Exception as e:  # a bug in a check is a failed step; the daemon is still shut down after it
            self.failures.append((name, repr(e)))
            print(f"FAIL {name} ({time.time() - t:.0f} s): {e!r}", flush=True)
            traceback.print_exc()
            return False

    # --- shared sequences

    def make_profile(self, pixel_um, extra_setup=None):
        """A new profile on the fake camera and mount, not connected. Returns its setup."""
        cli, fake = self.cli, self.fake
        base = cli.call("get_profile")
        name = f"e2e {self.name} {int(time.time())}"
        cli.call("create_profile", {"name": name, "select": True})
        self.profile = (base, name)
        cli.call("set_alpaca_server", {"host": fake.host, "port": fake.port, "camera_device": 0,
                                       "telescope_device": 0})
        cli.call("set_selected_camera", {"camera": camera_choice(fake)})
        cli.call("set_selected_mount", {"mount": mount_choice(fake)})
        cli.call("set_exposure", {"exposure": EXPOSURE_MS})
        params = {"pixel_size": pixel_um, "focal_length": FOCAL_MM}
        params.update(extra_setup or {})
        return cli.call("set_profile_setup", params)

    def restore_profile(self):
        cli = self.cli
        cli.call("stop_capture")
        cli.wait_state("Stopped", 30)
        cli.call("set_connected", {"connected": False}, timeout=60)
        base, name = self.profile
        cli.call("set_profile", {"id": base["id"]})
        cli.call("delete_profile", {"name": name, "delete_dark_files": True})
        self.profile = None
        return f"disconnected; profile {base.get('name')} again, {name} deleted"

    def calibrate(self, timeout):
        """loop, find_star, guide with recalibrate true to a settled guider. Returns the request's events."""
        e2e.loop_and_find_star(self.cli)
        evs, done = e2e.guide_with_recalibration(self.cli, 3, timeout)
        e2e.check_guide_request_order(evs)
        return evs, done

    def known_start(self, guiding):
        """After a step that failed part-way (#233): the fake up, the camera and mount connected, no Guiding
        Assistant run, and guiding on the kept calibration (guiding true) or stopped, so the next scenario runs from
        where it would have on a passing lane. Returns what it did."""
        cli, did = self.cli, []
        if self.fake.proc.poll() is not None:
            self.start_fake()
            did.append("fake restarted")
        else:
            # a scenario that failed part-way can leave a fault injected or a setting changed
            self.fake.request("PUT", "/fake/clear_faults")
            did.append("fake faults cleared")
        cli.call("set_variable_delay_settings", {"Enabled": False, "ShortDelaySeconds": 1, "LongDelaySeconds": 2})
        for device in ("camera", "mount"):
            if cli.call("get_connected", {"device": device}) is not True:
                cli.call("set_connected", {"device": device, "connected": True}, timeout=60)
                expect(cli.call("get_connected", {"device": device}) is True, f"the {device} did not connect")
                did.append(f"{device} connected")
        if cli.call("guiding_assistant_get_status").get("state") in ("measuring", "measuring_backlash"):
            cli.call("guiding_assistant_stop")
            did.append("Guiding Assistant stopped")
        cli.call("set_guide_output_enabled", {"enabled": True})
        state = cli.call("get_app_state")
        if state == ("Guiding" if guiding else "Stopped"):
            return did
        cli.call("stop_capture")
        cli.wait_state("Stopped", 30)
        did.append(f"{state} stopped")
        if guiding:
            e2e.loop_and_find_star(cli)
            start = cli.mark()
            expect(cli.call("guide", {"settle": e2e.SETTLE, "recalibrate": False}) == 0, "guide did not return 0")
            done = cli.wait_events(start, "SettleDone", timeout=e2e.SETTLE["timeout"] + 60, abort_on=GUIDE_FAILURES)
            expect(done[0].get("Status") == 0, f"SettleDone {strip(done[0])}")
            did.append("guiding on the kept calibration")
        return did

    def step_from_known_start(self, name, fn, guiding=True):
        """step(name, fn), after known_start once a step has failed (#233). One that cannot be reached skips the
        scenario with the reason, so a failure is reported once, not again by every scenario after it."""
        if self.failures:
            try:
                did = self.known_start(guiding)
            except e2e.RunTimeout:
                raise
            except Exception as e:
                return self.skip(name, f"no known start after {self.failures[-1][0]!r} failed: {e}")
            if did:
                print(f"e2e: known start for {name}: {', '.join(did)}", flush=True)
        return self.step(name, fn)

    def skip(self, name, reason):
        self.skipped.append((name, reason))
        print(f"SKIP {name}: {reason}", flush=True)
        return False

    def wait_reconnect_window(self):
        """Wait until no camera reconnect is left in the 60 s throttle window (MyFrame::DoTryReconnect)."""
        disc = self.cli.events_since(0, ("EquipmentDisconnected",))
        if disc:
            left = disc[-1]["_t"] + RECONNECT_WINDOW_S + 2 - time.time()
            if left > 0:
                print(f"e2e: waiting {left:.0f} s for the camera reconnect window", flush=True)
                time.sleep(left)


# ---------------------------------------------------------------------------------------------------------------
# the gear lane


def scenario_connect_side_effects(lane):
    """set_connected, get_connected, get_current_equipment and the events; the #139 latch; the refusals."""
    cli, fake = lane.cli, lane.fake
    out = []
    before = lane.make_profile(WRONG_PIXEL_UM)
    expect(before["pixel_size"] == WRONG_PIXEL_UM, f"set_profile_setup pixel_size reads {before['pixel_size']}")
    cal_step0 = before["calibration_duration"]

    # not connected: what Ara reads before the first connect
    expect(connected_by_device(cli) == {"all": False, "camera": False, "mount": False, "rotator": False},
           f"get_connected before connecting: {connected_by_device(cli)}")
    eq = equipment(cli)
    expect(eq == {"camera": (camera_choice(fake), False), "mount": (mount_choice(fake), False)},
           f"get_current_equipment before connecting: {eq}")
    expect(cli.call("get_pixel_scale") is not None, "get_pixel_scale null with a focal length and pixel size set")
    scale0 = cli.call("get_pixel_scale")
    want0 = 206.265 * WRONG_PIXEL_UM / FOCAL_MM
    expect(abs(scale0 - want0) < 0.01, f"get_pixel_scale {scale0} before connecting, expected {want0:.3f} "
                                       "(the profile's pixel size)")

    # the new profile's MinMoves, read before the connect: the guide algorithms' own default, not the image scale's
    mm0 = min_moves(cli)
    want_mm1 = smart_default_min_move(FAKE_PIXEL_UM)
    expect(all(abs(m - want_mm1) > 0.01 for m in mm0),
           f"MinMoves {mm0} before connecting already the new scale's default {want_mm1:.4f}: the reset is not seen")

    # connect all: the camera's pixel size is not the profile's, so the #139 adjustment is made, once
    start = cli.mark()
    expect(cli.call("set_connected", {"connected": True}, timeout=60) == 0, "set_connected true did not return 0")
    evs = settle_events(cli, start)
    names = names_of(evs)
    scale_alerts = alerts(evs, SCALE_ALERT)
    expect(len(scale_alerts) == 1, f"{len(scale_alerts)} pixel-size Alerts for the connect, expected 1: {names}")
    expect(not [e for e in evs if e["Event"].startswith("Equipment")], f"Equipment* events on a first connect: {names}")
    expect(names == ["ConfigurationChange", "Alert"], f"events of a first connect: {names}, expected "
                                                      "['ConfigurationChange', 'Alert']")
    expect(connected_by_device(cli) == {"all": True, "camera": True, "mount": True, "rotator": False},
           f"get_connected after connecting: {connected_by_device(cli)}")
    eq = equipment(cli)
    expect(eq == {"camera": (camera_connected_name(fake), True), "mount": (mount_connected_name(fake), True)},
           f"get_current_equipment after connecting: {eq}")
    after = setup(cli)
    expect(after["pixel_size"] == FAKE_PIXEL_UM, f"profile pixel_size {after['pixel_size']} after the connect, "
                                                 f"expected the camera's {FAKE_PIXEL_UM}")
    scale1 = cli.call("get_pixel_scale")
    want1 = 206.265 * FAKE_PIXEL_UM / FOCAL_MM
    expect(abs(scale1 - want1) < 0.01, f"get_pixel_scale {scale1} after connecting, expected {want1:.3f}")
    cal_step1 = after["calibration_duration"]
    want_step1 = calibration_step_ms(sidereal_multiple(1.0))
    expect((cal_step0, cal_step1) == (750, want_step1),
           f"calibration_duration {cal_step0} -> {cal_step1}, expected 750 -> {want_step1} (a new profile's default, "
           "then the new image scale's step at the mount's 1x; #115 N4)")
    mm1 = min_moves(cli)
    expect(all(abs(m - want_mm1) < 1e-3 for m in mm1),
           f"MinMoves {mm0} -> {mm1} after the scale change, expected both reset to the new scale's default "
           f"{want_mm1:.4f} (#139)")
    expect(cli.call("get_calibrated") is False, "calibrated on a new profile")
    out.append(f"connect: events {names}, 1 scale Alert, calibration_duration {cal_step0} -> {cal_step1}, "
               f"MinMove {mm0[0]:.4f}/{mm0[1]:.4f} -> {mm1[0]:.4f}/{mm1[1]:.4f}, pixel scale {scale0:.3f} -> "
               f"{scale1:.3f}\"/px")

    # what the client sets since is kept by every later request (#139)
    cli.call("set_algo_param", {"axis": "ra", "name": "minMove", "value": 0.42})
    cli.call("set_algo_param", {"axis": "dec", "name": "minMove", "value": 0.37})
    start = cli.mark()
    expect(cli.call("set_connected", {"connected": False}, timeout=60) == 0, "set_connected false did not return 0")
    evs = settle_events(cli, start)
    expect(connected_by_device(cli) == {"all": False, "camera": False, "mount": False, "rotator": False},
           f"get_connected after disconnecting: {connected_by_device(cli)}")
    eq = equipment(cli)
    expect(eq == {"camera": (camera_connected_name(fake), False), "mount": (mount_connected_name(fake), False)},
           f"get_current_equipment after disconnecting: {eq}")
    disconnect_names = names_of(evs)
    expect(disconnect_names == [], f"events on a disconnect: {disconnect_names}, expected none")
    cli.call("set_profile_setup", {"calibration_duration": 1234})
    for label, params in (("all", {}), ("camera", {"device": "camera"}), ("mount", {"device": "mount"})):
        for connect in (True, False, True) if label == "all" else (False, True):
            start = cli.mark()
            expect(cli.call("set_connected", dict(params, connected=connect), timeout=60) == 0,
                   f"set_connected {connect} {label} did not return 0")
            evs = settle_events(cli, start, quiet_s=0.5)
            expect(not alerts(evs), f"Alert on set_connected {connect} {label}: {names_of(evs)}")
            expect(cli.call("get_connected", {"device": label}) is connect,
                   f"get_connected {label} after set_connected {connect}")
    mm2 = min_moves(cli)
    expect(mm2 == (0.42, 0.37), f"MinMoves {mm2} after the reconnects, set 0.42 / 0.37 (#139)")
    expect(setup(cli)["calibration_duration"] == 1234, f"calibration_duration {setup(cli)['calibration_duration']} "
                                                       "after the reconnects, set 1234 (#139)")
    out.append(f"disconnect: events {disconnect_names}; reconnects all and per device kept MinMove 0.42/0.37 and "
               "calibration_duration 1234, no Alert")

    # set_connected's own refusals
    out.append(expect_error(cli, "set_connected", {"connected": True, "device": "rotator"}, 1,
                            "no rotator is selected in the profile"))
    expect(cli.call("set_connected", {"connected": False, "device": "rotator"}) == 0,
           "disconnecting the rotator nobody selected is not a no-op")
    code, msg = call_error(cli, "set_connected", {"connected": True, "device": "focuser"})
    expect(code == -32602, f"set_connected device focuser: {code} {msg}")

    # create_profile and clone_profile select the new profile unless select is false: while connected they refuse
    # with set_profile's refusal before creating anything (#185), so a retry with select false creates it
    other_name = f"e2e gear other {int(time.time())}"
    current_name = cli.call("get_profile")["name"]

    def named(name):
        return [p for p in cli.call("get_profiles") if p.get("name") == name]

    expect_error(cli, "create_profile", {"name": other_name}, 1, "cannot set profile when equipment is connected")
    expect(named(other_name) == [], f"create_profile refused while connected but created {named(other_name)} (#185)")
    expect_error(cli, "clone_profile", {"source_name": current_name, "dest_name": other_name}, 1,
                 "cannot set profile when equipment is connected")
    expect(named(other_name) == [], f"clone_profile refused while connected but created {named(other_name)} (#185)")
    code, msg = call_error(cli, "create_profile", {"name": other_name, "select": "yes"})
    expect(code == -32602 and named(other_name) == [],
           f"create_profile with a bad select: {code} {msg}, created {named(other_name)} (#185)")
    made = cli.call("create_profile", {"name": other_name, "select": False})
    expect(made.get("name") == other_name and made.get("selected") is False,
           f"create_profile select false while connected: {made}")
    other_id = made["id"]
    expect([p["id"] for p in named(other_name)] == [other_id], f"the profile create_profile made: {named(other_name)}")
    expect(cli.call("get_profile")["name"] == current_name, "create_profile switched the profile while connected")
    out.append("create_profile and clone_profile while connected: refused (\"cannot set profile when equipment is "
               "connected\") with nothing created; with select false created, not selected")

    # refused while equipment is connected; a profile switch among them
    refusals = (
        ("set_selected_camera", {"camera": camera_choice(fake)}, "cannot change camera selection while equipment "
                                                                 "is connected"),
        ("set_selected_mount", {"mount": mount_choice(fake)}, "cannot change mount selection while equipment is "
                                                              "connected"),
        ("set_alpaca_server", {"port": fake.port}, "cannot change Alpaca server while equipment is connected"),
        ("set_profile_setup", {"focal_length": 500}, "cannot change setup profile settings while equipment is "
                                                     "connected"),
        ("set_profile", {"id": other_id}, "cannot set profile when equipment is connected"),
        ("delete_profile", {"id": other_id}, "cannot delete profile while equipment is connected"),
    )
    for method, params, text in refusals:
        expect_error(cli, method, params, 1, text)
    # with the camera alone connected too: any connected device refuses them
    cli.call("set_connected", {"connected": False, "device": "mount"})
    expect_error(cli, "set_selected_mount", {"mount": mount_choice(fake)}, 1,
                 "cannot change mount selection while equipment is connected")
    expect_error(cli, "set_profile", {"id": other_id}, 1, "cannot set profile when equipment is connected")
    cli.call("set_connected", {"connected": True, "device": "mount"}, timeout=60)
    current = cli.call("get_profile")
    expect(cli.call("set_profile", {"id": current["id"]}) == 0, "set_profile to the current profile while "
                                                                 "connected is not a no-op")
    expect(cli.call("get_profile") == current and cli.call("get_connected") is True,
           "set_profile to the current profile changed the profile or the connection")
    out.append(f"refused while connected: {', '.join(m for m, _, _ in refusals)}; set_profile to the current id "
               "returns 0")

    # while looping: a disconnect of all or of the camera is refused, the mount's is not
    start = cli.mark()
    expect(cli.call("loop") == 0, "loop did not return 0")
    cli.wait_events(start, "LoopingExposures", count=2, timeout=60, abort_on=("LoopingExposuresStopped",))
    expect_error(cli, "set_connected", {"connected": False}, 1, "cannot disconnect equipment while capture active")
    code, cam_refusal = call_error(cli, "set_connected", {"connected": False, "device": "camera"})
    expect(code == 1, f"camera disconnect while looping: {code} {cam_refusal}")
    expect(cli.call("set_connected", {"connected": False, "device": "mount"}) == 0,
           "the mount disconnect while looping was refused")
    expect(cli.call("get_connected", {"device": "mount"}) is False, "the mount still connected")
    expect(cli.call("set_connected", {"connected": True, "device": "mount"}, timeout=60) == 0,
           "the mount connect while looping was refused")
    mark = cli.mark()
    cli.wait_events(mark, "LoopingExposures", count=2, timeout=60, abort_on=("LoopingExposuresStopped",))
    expect(cli.call("get_app_state") == "Looping", "not looping after the mount's disconnect and connect")
    cli.call("stop_capture")
    cli.wait_state("Stopped", 30)
    lane.other_profile = other_id  # deleted once disconnected, by the next scenario
    out.append(f"looping: all and camera disconnects refused ({cam_refusal!r}), the mount's disconnect and "
               "connect accepted, the loop goes on")
    return "; ".join(out)


def scenario_camera_change_mount_off(lane, args):
    """#139: a camera change seen while the mount is not connected clears the calibration when it connects."""
    cli, fake = lane.cli, lane.fake
    out = []
    cli.call("set_connected", {"connected": False}, timeout=60)
    cli.call("delete_profile", {"id": lane.other_profile})
    cli.call("set_profile_setup", dict(QUICK_CALIBRATION, auto_restore_calibration=True))
    cli.call("set_connected", {"connected": True}, timeout=60)
    evs, _ = lane.calibrate(args.calibration_timeout)
    legs = e2e.calibration_summary(evs)
    cli.call("stop_capture")
    cli.wait_state("Stopped", 30)
    expect(cli.call("get_calibrated") is True, "not calibrated after the calibration")
    out.append(f"calibrated ({legs})")

    # a reconnect with the camera unchanged auto-loads the saved calibration
    cli.call("set_connected", {"connected": False}, timeout=60)
    cal_off = cli.call("get_calibrated")
    cli.call("set_connected", {"connected": True}, timeout=60)
    expect(cli.call("get_calibrated") is True, "the saved calibration was not auto-loaded on a reconnect")
    out.append(f"get_calibrated {cal_off} disconnected, true after a reconnect (auto-load)")

    # the image scale changes while the mount refuses to connect
    cli.call("set_connected", {"connected": False}, timeout=60)
    cli.call("set_profile_setup", {"pixel_size": WRONG_PIXEL_UM})
    fake.request("PUT", "/fake/unplug", {"device": "telescope", "unplugged": "true"})
    start = cli.mark()
    code, msg = call_error(cli, "set_connected", {"connected": True}, timeout=60)
    expect(code == 1 and msg.startswith("equipment failed to connect: mount (") and "did not reply" in msg and
           msg.endswith(" (Alpaca 1280))"), f"set_connected with the mount refusing: {code} {msg!r}")
    evs = settle_events(cli, start)
    expect(connected_by_device(cli) == {"all": False, "camera": True, "mount": False, "rotator": False},
           f"get_connected with the mount refusing: {connected_by_device(cli)}")
    expect(len(alerts(evs, SCALE_ALERT)) == 1, f"pixel-size Alerts: {names_of(evs)}")
    refused_names = names_of(evs)
    cal_mount_off = cli.call("get_calibrated")
    out.append(f"connect all with the mount refusing: {msg!r}, events {refused_names}, get_calibrated "
               f"{cal_mount_off}")

    # the mount back, connected by a per-device request: the calibration is cleared now, not auto-loaded
    fake.request("PUT", "/fake/unplug", {"device": "telescope", "unplugged": "false"})
    start = cli.mark()
    expect(cli.call("set_connected", {"connected": True, "device": "mount"}, timeout=60) == 0,
           "the mount did not connect once the fake took it again")
    evs = settle_events(cli, start)
    rec = [e for e in evs if e["Event"] == "EquipmentReconnected"]
    expect([e.get("device_type") for e in rec] == ["mount"], f"EquipmentReconnected for the mount: {names_of(evs)}")
    expect(cli.call("get_calibrated") is False, "the calibration made at the old image scale is still loaded")
    expect(not alerts(evs, SCALE_ALERT), f"a second pixel-size Alert: {names_of(evs)}")
    for _ in range(2):  # the mount again, and all: still not auto-loaded in this process
        cli.call("set_connected", {"connected": False, "device": "mount"}, timeout=60)
        cli.call("set_connected", {"connected": True, "device": "mount"}, timeout=60)
    cli.call("set_connected", {"connected": False}, timeout=60)
    cli.call("set_connected", {"connected": True}, timeout=60)
    expect(cli.call("get_calibrated") is False, "the old calibration auto-loaded on a later reconnect")
    out.append(f"mount connected alone: events {names_of(evs)}, calibration cleared (get_calibrated false), not "
               "auto-loaded on later reconnects")

    # a new calibration, made at the new scale, is kept by later reconnects (#139)
    evs, _ = lane.calibrate(args.calibration_timeout)
    cli.call("stop_capture")
    cli.wait_state("Stopped", 30)
    for params in ({}, {"device": "camera"}, {"device": "mount"}):
        cli.call("set_connected", dict(params, connected=False), timeout=60)
        cli.call("set_connected", dict(params, connected=True), timeout=60)
    expect(cli.call("get_calibrated") is True, "the new calibration was cleared by a reconnect")
    out.append("recalibrated; kept by reconnects all and per device")
    return "; ".join(out)


# --- #115 N4: the headless image-scale adjustments (MyFrame::HandleImageScaleChange ->
# MyFrame::MakeImageScaleAdjustments / DetermineGuideSpeed, which S4.1 moved out of AdvancedDialog)

SIDEREAL_SECS_PER_SEC = 0.9973  # image_scale::SiderealRateFromGuideSpeed (src/image_scale.h) and Scope::ValidGuideRates
DEFAULT_GUIDE_SPEED = 0.5  # DetermineGuideSpeed's value when it has nothing better
SCALE_STEP_LINE = r"Cal step-size changed from (\d+) ms to (\d+) ms"
# MakeImageScaleAdjustments's first line, and its line when there is no scope or camera: never written on the
# headless path, where MyFrame::HandleImageScaleChange calls it only with a scope, and a camera is connected
SCALE_ADJUST_LINE = "Image scale has changed via AD UI - step-size and algo adjustments will be made"
SCALE_SKIP_LINE = "Image scale changed but scope/camera unavailable; skipping adjustments"
GUIDE_RATES_LINE = r"ScopeAlpaca::GetGuideRates returns (\d) ([\d.]+) ([\d.]+) a-s/sec"
INVALID_RATES_ALERT = "reporting invalid guide speeds"
BINNING_LINE = r"Binning (\d+) -> (\d+), rates \(([\d.]+), ([\d.]+)\) -> \(([\d.]+), ([\d.]+)\)"


def sidereal_multiple(rate_x):
    """SiderealRateFromGuideSpeed of a guide rate of rate_x * 15"/s, as the mount reports it in deg/s."""
    return rate_x * 15.0 / (15.0 * SIDEREAL_SECS_PER_SEC)


def calibration_step_ms(guide_speed, pixel_um=FAKE_PIXEL_UM, focal_mm=FOCAL_MM, binning=1):
    """CalstepDialog::GetCalibrationStepSize (src/calstep_dialog.cpp) at Dec 0 with DEFAULT_STEPS (12) over
    GetCalibrationDistance: the calibration step an image scale change sets for a guide speed (x sidereal). 300 ms
    for the fake's 3.75 um at 400 mm and 1x, 550 ms at the 0.5x default."""
    scale = 206.265 * pixel_um * binning / focal_mm
    distance = math.ceil(max(25.0, 20.0 / scale))
    total_s = distance * scale / (15.0 * guide_speed)
    pulse_ms = min(total_s / 6.0 * 1000.0, total_s / 12 * 1000.0)
    return int(math.ceil(pulse_ms / 50.0)) * 50


def scale_change_connect(lane, before_ms):
    """Disconnect, put the #139 wrong pixel size and a known calibration step in the profile, connect all: the
    latch's HandleImageScaleChange runs in GearSession::End. Returns (the connect's response, its events, the
    debug log since the connect)."""
    cli = lane.cli
    cli.call("set_connected", {"connected": False}, timeout=60)
    cli.call("set_profile_setup", {"pixel_size": WRONG_PIXEL_UM, "calibration_duration": before_ms})
    lane.debug.mark()
    start = cli.mark()
    msg = cli.request("set_connected", {"connected": True}, 60)
    evs = settle_events(cli, start)
    return msg, evs, lane.debug.text()


def check_scale_adjustment(lane, label, before_ms, want_speed, rates_line):
    """One image scale change through the #139 latch: one scale Alert, the calibration step recomputed for
    want_speed and both MinMoves reset (MakeImageScaleAdjustments), the calibration cleared, and the guide-rate read
    the debug log shows (rates_line: GetGuideRates's '<error> <dec> <ra>' line, None for no read)."""
    cli = lane.cli
    msg, evs, dbg = scale_change_connect(lane, before_ms)
    expect(msg.get("result") == 0, f"{label}: set_connected {msg}")
    expect(len(alerts(evs, SCALE_ALERT)) == 1, f"{label}: scale Alerts {names_of(evs)}, expected 1")
    want_ms = calibration_step_ms(want_speed)
    got_ms = setup(cli)["calibration_duration"]
    expect(got_ms == want_ms, f"{label}: calibration_duration {before_ms} -> {got_ms}, expected {want_ms} (guide "
                              f"speed {want_speed:.4f}x)")
    steps = re.findall(SCALE_STEP_LINE, dbg)
    expect(steps == [(str(before_ms), str(want_ms))], f"{label}: debug log step-size lines {steps}")
    mm = min_moves(cli)
    want_mm = smart_default_min_move(FAKE_PIXEL_UM)
    expect(all(abs(m - want_mm) < 1e-3 for m in mm), f"{label}: MinMoves {mm}, expected {want_mm:.4f}")
    expect(cli.call("get_calibrated") is False, f"{label}: still calibrated after the image scale change")
    # the read DetermineGuideSpeed made: the last GetGuideRates line before the step-size line
    head = dbg[:dbg.find("Cal step-size changed")]
    reads = re.findall(GUIDE_RATES_LINE, head)
    if rates_line is not None:
        expect(reads and reads[-1] == rates_line, f"{label}: GetGuideRates lines {reads}, expected the last "
                                                  f"{rates_line}")
    return f"{label}: {before_ms} -> {got_ms} ms ({want_speed:.3f}x)", evs


def scenario_guide_speed_sources(lane):
    """#115 N4: each branch of DetermineGuideSpeed, through the #139 latch's image scale change on connect."""
    cli, fake = lane.cli, lane.fake
    out = []
    expect(cli.call("get_calibrated") is True, "no calibration to start from (camera change with the mount off)")

    # the mount's guide rates read: the lower of RA and Dec, as a multiple of sidereal
    r, _ = check_scale_adjustment(lane, "mount at 1x", 1234, sidereal_multiple(1.0), ("0", "15.000", "15.000"))
    out.append(r)
    fake.request("PUT", "/fake/guiderate", {"ra": 1.0, "dec": 0.75})
    try:
        r, _ = check_scale_adjustment(lane, "mount at RA 1x, Dec 0.75x", 1234, sidereal_multiple(0.75),
                                      ("0", "11.250", "15.000"))
        out.append(r)

        # the mount's guide-rate read failing: the saved calibration's guide rates (1x, as calibrated), not the
        # mount's 0.75x. The failure at connect (ScopeAlpaca::Connect's probe) leaves the driver unable to read
        # them, so GetGuideRates fails with no request and no 'returns' line
        fake.request("PUT", "/fake/inject", {"device": "telescope", "call": "guideraterightascension",
                                             "error": 1024, "count": 1000})
        r, _ = check_scale_adjustment(lane, "guideraterightascension failing, saved calibration at 1x", 1234,
                                      sidereal_multiple(1.0), None)
        out.append(r)
        fake.request("PUT", "/fake/inject", {"device": "telescope", "call": "guideraterightascension", "count": 0})

        # rates the mount reports but Scope::ValidGuideRates refuses (2x): ScopeAlpaca::GetGuideRates fails on
        # them, with its invalid-speeds Alert, so this too takes the saved calibration's rates; the
        # ValidGuideRates check in DetermineGuideSpeed's read branch is never false for an Alpaca mount
        fake.request("PUT", "/fake/guiderate", {"ra": 2.0, "dec": 2.0})
        r, evs = check_scale_adjustment(lane, "mount at 2x (invalid), saved calibration at 1x", 1234,
                                        sidereal_multiple(1.0), None)
        invalid = alerts(evs, INVALID_RATES_ALERT)
        expect(len(invalid) == 1, f"invalid-speeds Alerts {names_of(evs)}, expected 1")
        out.append(f"{r}, 1 invalid-speeds Alert")
    finally:
        fake.request("PUT", "/fake/clear_faults")

    # the mount not connected: the 0.5x default (the mount refusing to connect, as in the camera change above)
    fake.request("PUT", "/fake/unplug", {"device": "telescope", "unplugged": "true"})
    try:
        msg, evs, dbg = scale_change_connect(lane, 1234)
    finally:
        fake.request("PUT", "/fake/unplug", {"device": "telescope", "unplugged": "false"})
    expect(msg.get("error", {}).get("code") == 1, f"set_connected with the mount refusing: {msg}")
    expect(len(alerts(evs, SCALE_ALERT)) == 1, f"mount off: scale Alerts {names_of(evs)}")
    want_ms = calibration_step_ms(DEFAULT_GUIDE_SPEED)
    got_ms = setup(cli)["calibration_duration"]
    expect(got_ms == want_ms, f"mount off: calibration_duration 1234 -> {got_ms}, expected {want_ms} (0.5x)")
    expect(re.findall(SCALE_STEP_LINE, dbg) == [("1234", str(want_ms))], "mount off: step-size lines "
                                                                          f"{re.findall(SCALE_STEP_LINE, dbg)}")
    out.append(f"mount not connected: 1234 -> {got_ms} ms (0.5x default)")

    # the guide-rate read failing with no saved calibration: the 0.5x default. On a profile of its own, which has
    # never calibrated
    cli.call("set_connected", {"connected": False}, timeout=60)
    keep, current = lane.profile, cli.call("get_profile")
    try:
        lane.make_profile(WRONG_PIXEL_UM)
        lane.profile = keep
        fake.request("PUT", "/fake/inject", {"device": "telescope", "call": "guideraterightascension",
                                             "error": 1024, "count": 1000})
        cli.call("set_profile_setup", {"calibration_duration": 1234})
        r, _ = check_scale_adjustment(lane, "guideraterightascension failing, no saved calibration", 1234,
                                      DEFAULT_GUIDE_SPEED, None)
        out.append(r)
    finally:
        fake.request("PUT", "/fake/clear_faults")
        cli.call("set_connected", {"connected": False}, timeout=60)
        made = cli.call("get_profile")
        if made["id"] != current["id"]:
            cli.call("set_profile", {"id": current["id"]})
            cli.call("delete_profile", {"id": made["id"]})

    # no mount selected: TheScope() is null, so MyFrame::HandleImageScaleChange does not call
    # MakeImageScaleAdjustments at all (its own no-scope skip line is not reached): the calibration step is kept,
    # and the Alert goes out
    cli.call("set_selected_mount", {"mount": "None"})
    try:
        msg, evs, dbg = scale_change_connect(lane, 1234)
        expect(msg.get("result") == 0, f"no mount: set_connected {msg}")
        expect(len(alerts(evs, SCALE_ALERT)) == 1, f"no mount: scale Alerts {names_of(evs)}")
        expect("gear_dialog: EndModal: imageScaleRatio changed" in dbg, "no mount: no latch line in the debug log")
        expect(SCALE_ADJUST_LINE not in dbg and SCALE_SKIP_LINE not in dbg,
               "no mount: MakeImageScaleAdjustments ran with no scope")
        got_ms = setup(cli)["calibration_duration"]
        expect(got_ms == 1234, f"no mount: calibration_duration 1234 -> {got_ms}, expected unchanged")
        out.append("no mount selected: MakeImageScaleAdjustments not called, calibration_duration 1234 kept, "
                   "1 scale Alert")
    finally:
        cli.call("set_connected", {"connected": False}, timeout=60)
        cli.call("set_selected_mount", {"mount": mount_choice(fake)})
        cli.call("set_profile_setup", dict(QUICK_CALIBRATION, pixel_size=FAKE_PIXEL_UM))
        cli.call("set_connected", {"connected": True}, timeout=60)
    return "; ".join(out)


def scenario_binning_change(lane, args):
    """#115 N4: Mount::AdjustCalibrationForScopePointing on a binning change: a calibration made at bin 1, the
    camera at bin 2 (software binning: the fake camera has no hardware binning), the calibration restored and guide
    with no recalibration.

    Pinned as it is today, which is not what the Alert says: the calibration is rescaled for bin 2 (and saved so),
    then MakeImageScaleAdjustments clears it, and guiding starts anyway, on a mount that reads uncalibrated, with
    no new calibration (Guider::UpdateGuideState is already past STATE_CALIBRATED when the adjustment runs). Only
    the next guide calibrates."""
    cli = lane.cli
    out = []
    evs, _ = lane.calibrate(args.calibration_timeout)
    cli.call("stop_capture")
    cli.wait_state("Stopped", 30)
    cal1 = cli.call("get_calibration_data", {"which": "Mount"})
    expect(cal1.get("calibrated") is True, f"not calibrated at bin 1: {cal1}")
    out.append(f"calibrated at bin 1 ({e2e.calibration_summary(evs)}, xRate {cal1['xRate']})")

    cli.call("set_connected", {"connected": False}, timeout=60)
    cli.call("set_profile_setup", {"software_binning": 2})
    try:
        # a binning change is not a pixel size change: no #139 latch, no Alert, the calibration auto-loaded
        start = cli.mark()
        cli.call("set_connected", {"connected": True}, timeout=60)
        evs = settle_events(cli, start)
        expect(cli.call("get_camera_binning") == 2, f"camera binning {cli.call('get_camera_binning')}, expected 2")
        expect(names_of(evs) == ["ConfigurationChange"], f"events of the connect at bin 2: {names_of(evs)}")
        expect(cli.call("get_calibrated") is True, "the bin 1 calibration was not auto-loaded at bin 2")
        expect(cli.call("restore_calibration") is True, "restore_calibration did not return true")
        out.append("connect at bin 2: no Alert, the bin 1 calibration auto-loaded; restore_calibration true")

        e2e.loop_and_find_star(cli)
        lane.debug.mark()
        start = cli.mark()
        expect(cli.call("guide", {"settle": e2e.SETTLE, "recalibrate": False}) == 0, "guide did not return 0")
        done = cli.wait_events(start, "SettleDone", timeout=e2e.SETTLE["timeout"] + 60, abort_on=GUIDE_FAILURES)[0]
        evs = cli.events_since(start)
        names = names_of(evs)
        expect(len(alerts(evs, SCALE_ALERT)) == 1, f"scale Alerts on the guide start: {names}, expected 1")
        expect(names == ["ConfigurationChange", "Alert", "StartGuiding", "SettleBegin", "SettleDone"],
               f"events of the guide: {names}")
        expect(done.get("Status") == 0, f"SettleDone {strip(done)}")
        dbg = lane.debug.text()
        lines = re.findall(BINNING_LINE, dbg)
        expect(len(lines) == 1 and lines[0][:2] == ("1", "2"), f"binning lines {lines}")
        old, new = [float(v) for v in lines[0][2:4]], [float(v) for v in lines[0][4:6]]
        expect(all(abs(n - o / 2) < 0.002 for o, n in zip(old, new)), f"rates {old} -> {new}, expected halved")
        expect("Mount::AdjustCalibrationForScopePointing: imageScaleRatio changed" in dbg,
               "no imageScaleRatio line from AdjustCalibrationForScopePointing")
        want_ms = calibration_step_ms(sidereal_multiple(1.0), binning=2)
        steps = re.findall(SCALE_STEP_LINE, dbg)
        expect(steps == [(str(QUICK_CALIBRATION["calibration_duration"]), str(want_ms))],
               f"step-size lines {steps}, expected 2500 -> {want_ms} (1x at bin 2)")
        expect(setup(cli)["calibration_duration"] == want_ms, f"calibration_duration {setup(cli)}")
        want_mm = smart_default_min_move(FAKE_PIXEL_UM, binning=2)
        mm = min_moves(cli)
        expect(all(abs(m - want_mm) < 1e-3 for m in mm), f"MinMoves {mm}, expected {want_mm:.4f} (bin 2)")
        # today: guiding on, the calibration cleared, and no calibration run
        expect(cli.call("get_app_state") == "Guiding", f"app state {cli.call('get_app_state')}")
        expect(cli.call("get_calibrated") is False, "still calibrated after the binning change")
        out.append(f"guide (no recalibration): events {names}, rates {old} -> {new}, step 2500 -> {want_ms} ms, "
                   f"MinMoves {want_mm:.4f}; guiding uncalibrated, no calibration run")

        # the next guide calibrates: stopped as soon as it starts (at bin 2 the fake's 1x moves the star 0.97 px/s,
        # a quarter of what the guider's 1.93"/px x 2 predicts, so a full calibration would take minutes)
        cli.call("stop_capture")
        cli.wait_state("Stopped", 30)
        e2e.loop_and_find_star(cli)
        start = cli.mark()
        expect(cli.call("guide", {"settle": e2e.SETTLE, "recalibrate": False}) == 0, "guide did not return 0")
        cli.wait_events(start, "StartCalibration", timeout=60, abort_on=GUIDE_FAILURES)
        out.append("the next guide (no recalibration) starts a calibration")
    finally:
        cli.call("stop_capture")
        cli.wait_state("Stopped", 30)
        cli.call("set_connected", {"connected": False}, timeout=60)
        cli.call("set_profile_setup", dict(QUICK_CALIBRATION, software_binning=1))
        cli.call("set_connected", {"connected": True}, timeout=60)
    return "; ".join(out)


def scenario_auto_connect(lane):
    """--auto-connect against the running fake, as the systemd unit starts the daemon."""
    lane.end_daemon()
    t0 = time.time()
    cli = lane.start_daemon(extra_args=("--auto-connect",))
    up = time.time() - t0
    first = connected_by_device(cli)
    expect(first == {"all": True, "camera": True, "mount": True, "rotator": False},
           f"get_connected when the RPC port first answers: {first}")
    evs = settle_events(cli, 0)
    names = [e["Event"] for e in evs]
    expect(not [n for n in names if n.startswith("Equipment")], f"Equipment* events after an auto-connect: {names}")
    expect(names == ["Version", "CalibrationComplete", "AppState"],
           f"events on connecting after an auto-connect that loaded a calibration: {names}")
    expect(cli.call("get_calibrated") is True, "the saved calibration was not auto-loaded at startup")
    expect("headless: auto-connect succeeded" in lane.debug.text(), "no 'auto-connect succeeded' in the debug log")
    return (f"RPC port up after {up:.0f} s with everything connected; events on connecting {names} (the auto-loaded "
            "calibration as a CalibrationComplete); calibration auto-loaded")


def scenario_auto_connect_late_fake(lane):
    """--auto-connect with the Alpaca server not up yet: the #104 retry connects it once it is."""
    cli = lane.cli
    lane.end_daemon()
    lane.stop_fake()
    t0 = time.time()
    cli = lane.start_daemon(extra_args=("--auto-connect",))
    dbg = lane.debug
    first = connected_by_device(cli)
    expect(first == {"all": False, "camera": False, "mount": False, "rotator": False},
           f"get_connected with no Alpaca server: {first}")
    eq = equipment(cli)
    expect({k: v[1] for k, v in eq.items()} == {"camera": False, "mount": False},
           f"get_current_equipment with no Alpaca server: {eq}")
    expect(cli.call("get_app_state") == "Stopped", "not Stopped")
    failed = dbg.wait(r"headless: auto-connect failed: (.*)", 30, lane.daemon)[0]
    expect(failed.startswith("equipment failed to connect: camera") and " mount" in failed,
           f"auto-connect failure: {failed!r}")
    # the first retry 15 s after the failed connect, the second 30 s after the first
    first_fail = dbg.wait(r"connect retry: the (camera|mount) failed to connect \(.*\); next attempt in (\d+) s "
                          r"\(failure 1\)", 30, lane.daemon, count=2)
    expect(sorted(first_fail) == [("camera", str(RETRY_FIRST_S)), ("mount", str(RETRY_FIRST_S))],
           f"after the auto-connect: {first_fail}")
    t1 = time.time()
    second = dbg.wait(r"connect retry: the (camera|mount) failed to connect \(.*\); next attempt in (\d+) s "
                      r"\(failure 2\)", RETRY_FIRST_S + 30, lane.daemon, count=2)
    t2 = time.time()
    expect(sorted(second) == [("camera", str(RETRY_SECOND_S)), ("mount", str(RETRY_SECOND_S))],
           f"after the first retry: {second}")
    expect(RETRY_FIRST_S - 3 <= t2 - t1 <= RETRY_FIRST_S + 15, f"the first retry {t2 - t1:.0f} s after the failure")
    probes = dbg.wait(r"connect retry: attempt (\d) for the (camera|mount): probing the Alpaca server", 1, count=2)
    quiet = cli.events_since(0)
    expect(not alerts(quiet) and not [e for e in quiet if e["Event"].startswith("Equipment")],
           f"events during the failed retries: {names_of(quiet)}")
    # the server comes up; the third attempt connects both
    mark = cli.mark()
    lane.start_fake()
    t3 = time.time()
    rec = cli.wait_events(mark, "EquipmentReconnected", count=2, timeout=RETRY_SECOND_S + 60)
    t4 = time.time()
    devices = sorted(e.get("device_type") for e in rec)
    expect(devices == ["camera", "mount"], f"EquipmentReconnected for {devices}")
    expect(t4 - t2 >= RETRY_SECOND_S - 3, f"connected {t4 - t2:.0f} s after the second failure, before its "
                                          f"{RETRY_SECOND_S} s")
    expect(connected_by_device(cli) == {"all": True, "camera": True, "mount": True, "rotator": False},
           f"get_connected after the retry: {connected_by_device(cli)}")
    evs = settle_events(cli, mark)
    expect(not alerts(evs), f"Alerts on the retry's connect: {names_of(evs)}")
    return (f"failed: {failed!r}; retries logged 15 s then 30 s apart ({len(probes)} probes seen), no event or Alert; "
            f"fake up {t3 - t0:.0f} s after start; EquipmentReconnected {devices} {t4 - t3:.0f} s later "
            f"({t4 - t2:.0f} s after the 2nd failure); events {names_of(evs)}")


def config_path(lane):
    return os.path.join(lane.work, "home", f".OpenAstroPHD2-instance{lane.args.instance}")


def set_profile_entries(lane, profile_id, entries):
    """Edit the stopped daemon's config file (a wxFileConfig): set `key=value` in the group
    [profile/<profile_id>/<group>] for each (group, key) -> value of `entries`; each entry must already be there."""
    path = config_path(lane)
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    group, done = None, set()
    for i, line in enumerate(lines):
        m = re.match(r"^\[(.*)\]\s*$", line)
        if m:
            group = m.group(1)
            continue
        key = line.split("=", 1)[0]
        for (g, k), value in entries.items():
            if group == f"profile/{profile_id}/{g}" and key == k:
                lines[i] = f"{k}={value}"
                done.add((g, k))
    missing = set(entries) - done
    expect(not missing, f"no {sorted(missing)} in profile {profile_id} of {path}")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def scenario_stale_selection(lane):
    """The profile file edited offline, as an upgrade or a hand edit leaves it, then a restart: a camera and a mount
    that are in no list, then a camera and a mount that differ from the lists only in case (#115 step 3, S2)."""
    cli, fake = lane.cli, lane.fake
    pid = cli.call("get_profile")["id"]
    out = []

    def restart_with(camera, mount):
        lane.cli.call("set_connected", {"connected": False}, timeout=60)
        lane.end_daemon()
        set_profile_entries(lane, pid, {("camera", "LastMenuChoice"): camera, ("scope", "LastMenuChoice"): mount})
        return lane.start_daemon()

    # In no list: LoadGearChoices finds no match, so the camera falls back to the first item of its (sorted) list,
    # the Alpaca camera on the profile's server, and the mount to '' (no mount device); both are written back
    cli = restart_with("Alpaca Camera [stale.invalid:1/0]", "Alpaca Mount [stale.invalid:1/0]")
    got = (cli.call("get_selected_camera"), cli.call("get_selected_mount"))
    expect(got == (camera_choice(fake), ""), f"get_selected_camera / _mount with a stale camera and mount: {got}")
    eq = equipment(cli)
    expect(eq == {"camera": (camera_choice(fake), False)}, f"get_current_equipment with a stale camera and mount: {eq}")
    created = lane.debug.wait(r"Created new scope of type (.*) = ", 10, lane.daemon)
    expect(created == [""], f"the mounts created at startup: {created}")
    expect(connected_by_device(cli) == {"all": False, "camera": False, "mount": False, "rotator": False},
           f"get_connected: {connected_by_device(cli)}")
    out.append(f"stale: camera {got[0]!r}, mount {got[1]!r}, equipment {eq}")

    # Only the case differs: wxChoice::SetStringSelection matches without case, so both resolve to the list's
    # item, and the list's casing is what is written back
    cli = restart_with(camera_choice(fake).swapcase(), mount_choice(fake).upper())
    got = (cli.call("get_selected_camera"), cli.call("get_selected_mount"))
    expect(got == (camera_choice(fake), mount_choice(fake)),
           f"get_selected_camera / _mount with the case changed: {got}")
    eq = equipment(cli)
    expect(eq == {"camera": (camera_choice(fake), False), "mount": (mount_choice(fake), False)},
           f"get_current_equipment with the case changed: {eq}")
    cli.call("set_connected", {"connected": True}, timeout=60)
    expect(connected_by_device(cli) == {"all": True, "camera": True, "mount": True, "rotator": False},
           f"get_connected after connecting: {connected_by_device(cli)}")
    out.append(f"case only: resolved to {got}, connected")
    return "; ".join(out)


def stopped_and_disconnected(cli):
    """No capture and nothing connected, whatever the scenario before left."""
    cli.call("stop_capture")
    cli.wait_state("Stopped", 30)
    cli.call("set_connected", {"connected": False}, timeout=60)


def free_port():
    """A local TCP port nothing listens on: a connect to it is refused at once."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def selections(cli):
    return cli.call("get_selected_camera"), cli.call("get_selected_mount")


def scenario_guide_connects_first(lane, args):
    """guide on a guider whose camera and mount are selected but were never connected (#104, the guide RPC's
    connect-first): both are connected first, then it calibrates and guides; after a user's disconnect guide refuses
    instead, naming the devices."""
    cli = lane.cli
    out = []
    stopped_and_disconnected(cli)
    lane.end_daemon()
    cli = lane.start_daemon()  # no --auto-connect: nothing connected and nothing asked for yet
    expect(connected_by_device(cli) == {"all": False, "camera": False, "mount": False, "rotator": False},
           f"get_connected after a start without --auto-connect: {connected_by_device(cli)}")
    start = cli.mark()
    t0 = time.time()
    expect(cli.call("guide", {"settle": e2e.SETTLE, "recalibrate": True}, timeout=60) == 0, "guide did not return 0")
    took = time.time() - t0
    # connected before guide returned: the connect-first runs inside the request
    expect(connected_by_device(cli) == {"all": True, "camera": True, "mount": True, "rotator": False},
           f"get_connected once guide returned: {connected_by_device(cli)}")
    first = lane.debug.wait(r"guide: the (camera|mount) is not connected; connecting it first", 5, lane.daemon, count=2)
    expect(first == ["camera", "mount"], f"the devices guide connected first: {first}")
    sc = cli.wait_events(start, "StartCalibration", timeout=60, abort_on=GUIDE_FAILURES)[0]
    cal_from = cli.events_since(start).index(sc) + start
    cli.wait_events(cal_from, "CalibrationComplete", timeout=args.calibration_timeout, abort_on=GUIDE_FAILURES)
    done = cli.wait_events(cal_from, "SettleDone", timeout=e2e.SETTLE["timeout"] + 30, abort_on=GUIDE_FAILURES)[0]
    expect(done.get("Status") == 0 and not done.get("Error"), f"SettleDone {strip(done)}")
    after = cli.events_since(start).index(done) + start + 1
    cli.wait_events(after, "GuideStep", count=3, timeout=60, abort_on=GUIDE_FAILURES)
    expect(cli.call("get_app_state") == "Guiding", "not guiding after the settle")
    before_cal = names_of(cli.events_since(start)[:cal_from - start])
    # the connects' own events, then the guide request's (the calibration the connect auto-loaded is replaced)
    expect(before_cal == ["ConfigurationChange", "LockPositionSet"],
           f"events between guide and StartCalibration: {before_cal}")
    e2e.check_guide_request_order(cli.events_since(cal_from))
    out.append(f"guide returned 0 after {took:.1f} s with the camera and mount connected first; events before the "
               f"calibration {before_cal}; calibrated, settled, guiding")

    # disconnected by the user: guide does not connect them again
    stopped_and_disconnected(cli)
    text = expect_error(cli, "guide", {"settle": e2e.SETTLE, "recalibrate": True}, 1,
                        "all equipment must be connected first: the camera was disconnected by the user; the mount "
                        "was disconnected by the user")
    expect(connected_by_device(cli)["all"] is False and cli.call("get_app_state") == "Stopped",
           "guide after a user disconnect connected or started something")
    out.append(f"after set_connected false: {text!r}")
    cli.call("set_connected", {"connected": True}, timeout=60)
    return "; ".join(out)


def scenario_alpaca_server_carry(lane):
    """set_alpaca_server with nothing connected (EquipmentManager's AlpacaServerChanging/Changed): the Alpaca
    selections follow the new server, nothing connects or disconnects, and a device still to be connected (#104) is
    tried again at once on the new server; a device the user disconnected is not."""
    try:
        return alpaca_server_carry(lane)
    finally:
        # the scenarios after it run on the fake, with its mount on, whatever failed here
        lane.fake.request("PUT", "/fake/unplug", {"device": "telescope", "unplugged": "false"})
        if not any(lane.cli.call("get_connected", {"device": d}) for d in ("camera", "mount")):
            lane.cli.call("set_alpaca_server", {"port": lane.fake.port})


def alpaca_server_carry(lane):
    cli, fake = lane.cli, lane.fake
    out = []
    stopped_and_disconnected(cli)
    # the mount fails to connect, so the #104 retry wants it; the camera connects, then the user disconnects it
    fake.request("PUT", "/fake/unplug", {"device": "telescope", "unplugged": "true"})
    code, msg = call_error(cli, "set_connected", {"connected": True}, timeout=60)
    expect(code == 1 and msg.startswith("equipment failed to connect: mount ("), f"connect all: {code} {msg!r}")
    cli.call("set_connected", {"connected": False, "device": "camera"}, timeout=60)
    expect(connected_by_device(cli) == {"all": False, "camera": False, "mount": False, "rotator": False},
           f"get_connected before set_alpaca_server: {connected_by_device(cli)}")
    expect(selections(cli) == (camera_choice(fake), mount_choice(fake)), f"selections: {selections(cli)}")

    # another server, with nothing on it: both Alpaca selections move there, the mount is tried there at once
    other = free_port()
    lane.debug.mark()
    mark = cli.mark()
    expect(cli.call("set_alpaca_server", {"port": other}) == 0, "set_alpaca_server to another port did not return 0")
    want = (f"Alpaca Camera [{fake.host}:{other}/0]", f"Alpaca Mount [{fake.host}:{other}/0]")
    expect(selections(cli) == want, f"selections after set_alpaca_server port {other}: {selections(cli)}")
    expect(connected_by_device(cli)["all"] is False, "something connected on set_alpaca_server")
    tried = lane.debug.wait(r"connect retry: the Alpaca server settings changed; the (\w+) is tried again now", 5,
                            lane.daemon)
    expect(tried == ["mount"], f"the devices tried again after the server change: {tried}")
    lane.debug.wait(rf"connect probe: mount at {re.escape(fake.host)}:{other} device 0", 15, lane.daemon)
    failed = lane.debug.wait(r"connect retry: the mount failed to connect \(.*\); next attempt in (\d+) s "
                             r"\(failure (\d+)\)", 30, lane.daemon)
    expect(failed == [(str(RETRY_FIRST_S), "1")], f"the mount's attempt on the new server: {failed}")
    evs = settle_events(cli, mark)
    names = names_of(evs)
    # the new mount choice builds a new mount, which sends its guide parameters
    expect(names == ["ConfigurationChange"] + ["GuideParamChange"] * 3,
           f"events on set_alpaca_server and the failed attempt: {names}")
    out.append(f"port {other}: selections {want}, the mount tried there at once and failed (next in "
               f"{RETRY_FIRST_S} s, failure 1), events {names}")

    # back to the fake, the mount powered on: the mount connects at once, the camera stays disconnected
    fake.request("PUT", "/fake/unplug", {"device": "telescope", "unplugged": "false"})
    lane.debug.mark()
    mark = cli.mark()
    expect(cli.call("set_alpaca_server", {"port": fake.port}) == 0, "set_alpaca_server back did not return 0")
    expect(selections(cli) == (camera_choice(fake), mount_choice(fake)), f"selections back: {selections(cli)}")
    rec = cli.wait_events(mark, "EquipmentReconnected", timeout=30)
    expect([e.get("device_type") for e in rec] == ["mount"], f"EquipmentReconnected {[strip(e) for e in rec]}")
    evs = settle_events(cli, mark)
    expect(connected_by_device(cli) == {"all": False, "camera": False, "mount": True, "rotator": False},
           f"get_connected after the mount's retry: {connected_by_device(cli)}")
    expect(not re.search(r"connect retry: attempt \d+ for the camera", lane.debug.text()),
           "the camera the user disconnected was tried")
    names = names_of(evs)
    expect(names == (["ConfigurationChange"] + ["GuideParamChange"] * 3 +
                     ["ConfigurationChange", "EquipmentReconnected"]),
           f"events of the change back and the mount's retry: {names}")
    out.append(f"port {fake.port} again: selections back, the mount reconnected at once (events {names}), the camera "
               "left disconnected")
    cli.call("set_connected", {"connected": True}, timeout=60)
    expect(cli.call("get_connected") is True, "not connected after connect all")
    return "; ".join(out)


# the fake holds the mount's refused Connected=true PUT this long, so the #104 probe is in flight at the shutdown
PROBE_HELD_MS = 6000


def scenario_shutdown_during_probe(lane):
    """shutdown while the #104 background probe for the mount waits on the server: a clean exit (status 0, no
    sanitizer report), as prompt as the probe's request allows, with no connect started after it."""
    cli, fake = lane.cli, lane.fake
    stopped_and_disconnected(cli)
    fake.request("PUT", "/fake/unplug", {"device": "telescope", "unplugged": "true", "delay_ms": str(PROBE_HELD_MS)})
    lane.debug.mark()
    code, msg = call_error(cli, "set_connected", {"connected": True, "device": "mount"}, timeout=60)
    expect(code == 1, f"the mount's connect: {code} {msg!r}")
    lane.debug.wait(r"connect retry: the mount failed to connect \(.*\); next attempt in \d+ s \(failure 1\)", 10,
                    lane.daemon)
    lane.debug.wait(r"connect retry: attempt \d+ for the mount: probing the Alpaca server", RETRY_FIRST_S + 30,
                    lane.daemon)
    lane.debug.wait(r"connect probe: mount at ", 5, lane.daemon)
    time.sleep(1.0)  # its Connected read has answered; the fake holds its Connected=true PUT
    text = lane.debug.text()
    expect(not re.search(r"connect probe: mount not connected", text), "the probe ended before the shutdown")
    t0 = time.time()
    status = lane.end_daemon()
    took = time.time() - t0
    limit = PROBE_HELD_MS / 1000.0 + 15
    expect(took < limit, f"the daemon took {took:.0f} s to exit, expected under {limit:.0f} s")
    text = lane.debug.text()
    stopped = text.find("connect retry: stopped (shutting down)")
    expect(stopped >= 0, "no 'connect retry: stopped (shutting down)' in the debug log")
    expect("connect retry: connecting the mount" not in text, "a background connect of the mount started")
    fake.request("PUT", "/fake/unplug", {"device": "telescope", "unplugged": "false", "delay_ms": "0"})
    lane.start_daemon()
    return f"shutdown with the probe in flight: {status} after {took:.1f} s; the retry stopped, no connect after it"


# ---------------------------------------------------------------------------------------------------------------
# the faults lane


def scenario_camera_drop_loop(lane):
    """While looping: a 1280 stops the loop; a 1031 reconnects the camera and the loop goes on."""
    cli, fake = lane.cli, lane.fake
    out = []
    start = cli.mark()
    expect(cli.call("loop") == 0, "loop did not return 0")
    cli.wait_events(start, "LoopingExposures", count=2, timeout=60, abort_on=("LoopingExposuresStopped",))
    mark = cli.mark()
    fake.inject("imagearray", 1280, 1, "e2e loop drop")
    cli.wait_events(mark, "LoopingExposuresStopped", timeout=60)
    evs = settle_events(cli, mark)
    drop_names = names_of(evs)
    # #186: a failed frame stops the loop with one LoopingExposuresStopped
    expect(drop_names == ["Alert", "LoopingExposuresStopped"],
           f"events of a 1280 while looping: {drop_names}, expected ['Alert', 'LoopingExposuresStopped']")
    expect(cli.call("get_app_state") == "Stopped", f"app state {cli.call('get_app_state')} after a 1280 while looping")
    expect(cli.call("get_connected") is True, "the camera disconnected after a 1280 while looping")
    out.append(f"1280 while looping: {drop_names}, Stopped")

    start = cli.mark()
    cli.call("loop")
    cli.wait_events(start, "LoopingExposures", count=2, timeout=60, abort_on=("LoopingExposuresStopped",))
    mark = cli.mark()
    fake.inject("imagearray", 1031, 1, "e2e camera not connected")
    rec = cli.wait_events(mark, "EquipmentReconnected", timeout=60, abort_on=("EquipmentReconnectFailed",))
    after = cli.events_since(mark).index(rec[0]) + mark + 1
    cli.wait_events(after, "LoopingExposures", count=2, timeout=60, abort_on=("LoopingExposuresStopped",))
    evs = cli.events_since(mark)
    disc = [e for e in evs if e["Event"] == "EquipmentDisconnected"]
    expect(len(disc) == 1 and disc[0].get("device_type") == "camera" and disc[0].get("reconnecting") is True,
           f"EquipmentDisconnected for a 1031: {[strip(e) for e in disc]}")
    expect(disc[0].get("reason") == "The Alpaca server reports the camera as not connected: e2e camera not connected",
           f"reason {disc[0].get('reason')!r}")
    expect(rec[0].get("device_type") == "camera", f"EquipmentReconnected {strip(rec[0])}")
    order = [n for n in names_of(evs) if n in ("EquipmentDisconnected", "EquipmentReconnected", "Alert")]
    expect(order == ["EquipmentDisconnected", "Alert", "EquipmentReconnected"],
           f"order {order}, expected ['EquipmentDisconnected', 'Alert', 'EquipmentReconnected']")
    expect(cli.call("get_app_state") == "Looping", "not looping after the camera reconnected")
    cli.call("stop_capture")
    cli.wait_state("Stopped", 30)
    out.append(f"1031 while looping: {order}, looping again")
    return "; ".join(out)


def scenario_mount_read_errors_calibration(lane, args):
    """declination and sideofpier failing for the whole calibration: it completes, Dec and pier side unknown."""
    cli, fake = lane.cli, lane.fake
    lane.debug.mark()
    for call in ("declination", "sideofpier"):
        fake.request("PUT", "/fake/inject", {"device": "telescope", "call": call, "error": 1024, "count": 100000,
                                             "message": f"e2e {call} read failed"})
    try:
        evs, done = lane.calibrate(args.calibration_timeout)
    finally:
        for call in ("declination", "sideofpier"):
            fake.request("PUT", "/fake/inject", {"device": "telescope", "call": call, "count": 0})
    legs = e2e.check_full_calibration(evs)
    cal = cli.call("get_calibration_data", {"which": "Mount"})
    expect(cal.get("calibrated") is True, f"not calibrated: {cal}")
    # the first calibration of a new profile turns auto-restore on (CheckCalibrationAutoLoad: the mount reports
    # its position), with no Alert
    expect(setup(cli)["auto_restore_calibration"] is True, "auto_restore_calibration still off after the new "
                                                           "profile's first calibration")
    # an unknown Dec and side of pier are null in get_calibration_data (#189), not UNKNOWN_DECLINATION (997 rad,
    # src/mount.h) through degrees(), 57123.9, and the field there, so Ara can tell the calibration lacks them
    expect("declination" in cal and cal["declination"] is None,
           f"calibration declination {cal.get('declination', '(missing)')!r}, expected null (unknown)")
    expect("pierSide" in cal and cal["pierSide"] is None,
           f"calibration pierSide {cal.get('pierSide', '(missing)')!r}, expected null (unknown)")
    # the driver's reads are in the debug log
    pier = set(lane.debug.wait(r"ScopeAlpaca::SideOfPier\(\) returns (-?\d+)", 1, count=1))
    expect(pier == {"-1"}, f"SideOfPier() returned {sorted(pier)} during the calibration, expected only -1 "
                           "(PIER_SIDE_UNKNOWN)")
    names = names_of(evs)
    expect(not alerts(evs), f"Alerts during the calibration: {names}")
    return (f"calibration completed ({legs}), get_calibration_data declination {cal['declination']!r} "
            f"pierSide {cal['pierSide']!r}, SideOfPier() {sorted(pier)}, no Alert; guiding")


def scenario_pulseguide_errors(lane):
    """pulseguide failing while guiding: one Alert, at most one every 30 s (#188), guiding goes on through fewer than
    10 failed moves in a row, the mount stays connected; the 10th failed move in a row stops guiding (#213)."""
    cli, fake = lane.cli, lane.fake
    expect(cli.call("get_app_state") == "Guiding", "not guiding")
    # bigger corrections, so every frame sends a pulse
    fake.request("PUT", "/fake/shift", {"dx": 4, "dy": 4})
    mark = cli.mark()
    log0 = fake_log_size(lane)
    lane.debug.mark()
    fake.request("PUT", "/fake/inject", {"device": "telescope", "call": "pulseguide", "error": 1280, "count": 100000,
                                         "message": "e2e pulseguide failed"})
    try:
        steps = cli.wait_events(mark, "GuideStep", count=6, timeout=90, abort_on=GUIDE_FAILURES)
    finally:
        fake.request("PUT", "/fake/inject", {"device": "telescope", "call": "pulseguide", "count": 0})
    evs = settle_events(cli, mark)
    failed = fake_log_since(lane, log0).count("injected error 1280 on /api/v1/telescope/0/pulseguide")
    failed_moves = len(re.findall(FAILED_MOVE_LINE, lane.debug.text()))
    # a shorter run of failed moves than the stop's keeps guiding (#213)
    expect(0 < failed_moves < MAX_FAILED_GUIDE_MOVES,
           f"{failed_moves} failed moves in the short run, expected 1 to {MAX_FAILED_GUIDE_MOVES - 1}")
    expect(not alerts(evs, FAILED_MOVES_ALERT), f"the failed-moves Alert after {failed_moves} failed moves")
    pulse_alerts = alerts(evs, PULSEGUIDE_ALERT)
    # throttled (#188): the first failed pulse raises the Alert, and then at most one every
    # PULSEGUIDE_ALERT_THROTTLE_S, not one per failed pulse (two a frame when both axes correct)
    expect(failed > 1 and pulse_alerts, f"{len(pulse_alerts)} PulseGuide Alerts for {failed} failed pulses")
    gaps = [b["_t"] - a["_t"] for a, b in zip(pulse_alerts, pulse_alerts[1:])]
    expect(all(g > PULSEGUIDE_ALERT_THROTTLE_S - 1 for g in gaps),
           f"{len(pulse_alerts)} PulseGuide Alerts for {failed} failed pulses, "
           f"{[round(g, 1) for g in gaps]} s apart, expected at most one every {PULSEGUIDE_ALERT_THROTTLE_S} s")
    expect(cli.call("get_app_state") == "Guiding", "not guiding after the pulseguide errors")
    expect(cli.call("get_connected", {"device": "mount"}) is True, "the mount disconnected")
    expect(not [e for e in evs if e["Event"].startswith("Equipment")], f"Equipment* events: {names_of(evs)}")
    # back to normal: corrections reach the mount again
    pulses0 = len(fake.request("GET", "/fake/pulses?clear=1")["pulses"])
    mark = cli.mark()
    fake.request("PUT", "/fake/shift", {"dx": 3, "dy": 3})
    cli.wait_events(mark, "GuideStep", count=4, timeout=60, abort_on=GUIDE_FAILURES)
    pulses = fake.request("GET", "/fake/pulses")["pulses"]
    expect(pulses, "no pulse reached the mount after the errors cleared")
    out = [f"{len(steps)} GuideSteps with pulseguide failing, {failed} failed pulses in {failed_moves} failed moves, "
           f"{len(pulse_alerts)} Alerts {pulse_alerts[0].get('Msg')!r}, guiding, the mount connected; {len(pulses)} "
           f"pulses after (before the clear {pulses0})"]

    # failing on: the 10th failed move in a row stops guiding, with an Alert naming the mount (#213)
    mark = cli.mark()
    lane.debug.mark()
    fake.request("PUT", "/fake/shift", {"dx": 4, "dy": 4})
    fake.request("PUT", "/fake/inject", {"device": "telescope", "call": "pulseguide", "error": 1280, "count": 100000,
                                         "message": "e2e pulseguide failed"})
    try:
        cli.wait_events(mark, "GuidingStopped", timeout=120, abort_on=("CalibrationFailed", "LoopingExposuresStopped"))
        evs = settle_events(cli, mark, quiet_s=2.0)
    finally:
        fake.request("PUT", "/fake/inject", {"device": "telescope", "call": "pulseguide", "count": 0})
    # the stop comes on the 10th failed move in a row, not later: the failed moves logged before the stop's own
    # Debug line (a move still in flight may finish after it)
    log = lane.debug.text()
    stop_line = re.search(STOP_LINE, log)
    expect(stop_line is not None, "no failed-moves stop in the debug log")
    failed_moves = len(re.findall(FAILED_MOVE_LINE, log[:stop_line.start()]))
    expect(failed_moves == MAX_FAILED_GUIDE_MOVES and int(stop_line.group(1)) == MAX_FAILED_GUIDE_MOVES,
           f"guiding stopped after {failed_moves} failed moves (the stop's line: {stop_line.group(0)!r}), expected "
           f"{MAX_FAILED_GUIDE_MOVES}")
    stop_alerts = alerts(evs, FAILED_MOVES_ALERT)
    expect(len(stop_alerts) == 1, f"{len(stop_alerts)} failed-moves Alerts: {names_of(evs)}")
    msg = stop_alerts[0].get("Msg", "")
    expect(mount_connected_name(fake) in msg, f"the failed-moves Alert {msg!r} does not name the mount "
                                              f"{mount_connected_name(fake)!r}")
    order = [("stop" if e is stop_alerts[0] else e["Event"]) for e in evs
             if e is stop_alerts[0] or e["Event"] == "GuidingStopped"]
    expect(order == ["stop", "GuidingStopped"], f"the failed-moves Alert and GuidingStopped: {order}")
    after = evs[evs.index(stop_alerts[0]) + 1:]
    expect(not [e for e in after if e["Event"] == "GuideStep"], "GuideSteps after guiding stopped")
    expect(not [e for e in evs if e["Event"].startswith("Equipment")], f"Equipment* events: {names_of(evs)}")
    state = cli.call("get_app_state")
    expect(state in ("Looping", "Selected", "Stopped"), f"app state {state} after the stop")
    expect(cli.call("get_connected", {"device": "mount"}) is True, "the mount disconnected")
    out.append(f"failing on: stopped after {failed_moves} failed moves, Alert {msg!r}, then GuidingStopped, {state}")

    # the pulses go through again: guide on the same calibration for the scenarios that follow
    start = cli.mark()
    expect(cli.call("guide", {"settle": e2e.SETTLE, "recalibrate": False}) == 0, "guide did not return 0")
    done = cli.wait_events(start, "SettleDone", timeout=e2e.SETTLE["timeout"] + 60, abort_on=GUIDE_FAILURES)[0]
    expect(done.get("Status") == 0, f"SettleDone {strip(done)}")
    cli.wait_events(start, "GuideStep", count=3, timeout=60, abort_on=GUIDE_FAILURES)
    out.append("guide settled again")
    return "; ".join(out)


def scenario_guide_output_off(lane):
    """Guide output turned off while guiding (set_guide_output_enabled false, as the Guiding Assistant does): each
    frame's guide move is refused, never sent to the mount, and is not a failed move (#213). Guiding goes on past
    MAX_FAILED_GUIDE_MOVES frames with no failed-moves Alert, and guides again once output is back on."""
    cli = lane.cli
    expect(cli.call("get_app_state") == "Guiding", "not guiding")
    frames = MAX_FAILED_GUIDE_MOVES + 5
    mark = cli.mark()
    expect(cli.call("set_guide_output_enabled", {"enabled": False}) == 0, "set_guide_output_enabled false failed")
    try:
        steps = cli.wait_events(mark, "GuideStep", count=frames, timeout=90, abort_on=GUIDE_FAILURES)
        evs = settle_events(cli, mark)
        stop_alerts = alerts(evs, FAILED_MOVES_ALERT)
        expect(not stop_alerts, f"failed-moves Alerts with guide output off: {[e.get('Msg') for e in stop_alerts]}")
        expect(cli.call("get_app_state") == "Guiding", "not guiding with guide output off")
    finally:
        cli.call("set_guide_output_enabled", {"enabled": True})
    mark = cli.mark()
    cli.wait_events(mark, "GuideStep", count=3, timeout=60, abort_on=GUIDE_FAILURES)
    return f"{len(steps)} GuideSteps with guide output off, no failed-moves Alert, guiding; output on again, guiding"


def scenario_third_drop(lane):
    """3 frames in a row failing with 1280 while guiding: 2 StarLost, then the #103 reconnect, guiding goes on."""
    cli, fake = lane.cli, lane.fake
    expect(cli.call("get_app_state") == "Guiding", "not guiding")
    mark = cli.mark()
    fake.inject("imagearray", 1280, 3, "e2e third drop")
    rec = cli.wait_events(mark, "EquipmentReconnected", timeout=90, abort_on=GUIDING_ENDED)
    after = cli.events_since(mark).index(rec[0]) + mark + 1
    cli.wait_events(after, "GuideStep", count=3, timeout=60, abort_on=GUIDING_ENDED)
    evs = cli.events_since(mark)
    names = names_of(evs)
    lost = [e for e in evs if e["Event"] == "StarLost"]
    disc = [e for e in evs if e["Event"] == "EquipmentDisconnected"]
    expect(len(lost) == 2, f"{len(lost)} StarLost for 3 dropped frames, expected 2 (the 3rd reconnects): {names}")
    expect(len(disc) == 1 and disc[0].get("reconnecting") is True, f"EquipmentDisconnected: {names}")
    want = "The camera failed 3 image acquisitions in a row (Alpaca error 1280: e2e third drop)"
    expect(disc[0].get("reason") == want, f"reason {disc[0].get('reason')!r}")
    seq = [n for n in names if n in ("StarLost", "EquipmentDisconnected", "EquipmentReconnected")]
    expect(seq == ["StarLost", "StarLost", "EquipmentDisconnected", "EquipmentReconnected"], f"order {seq}")
    expect(cli.call("get_app_state") == "Guiding", "not guiding after the reconnect")
    return f"{seq}, guiding (events {names})"


def scenario_capture_timeout(lane):
    """An exposure that never completes: the watchdog disconnects and reconnects the camera, guiding goes on."""
    cli, fake = lane.cli, lane.fake
    expect(cli.call("get_app_state") == "Guiding", "not guiding")
    cli.call("set_camera_timeout", {"timeout_ms": 5000})
    mark = cli.mark()
    fake.request("PUT", "/fake/hold", {"frames": 1})
    rec = cli.wait_events(mark, "EquipmentReconnected", timeout=90, abort_on=GUIDING_ENDED)
    after = cli.events_since(mark).index(rec[0]) + mark + 1
    cli.wait_events(after, "GuideStep", count=3, timeout=60, abort_on=GUIDING_ENDED)
    evs = cli.events_since(mark)
    names = names_of(evs)
    disc = [e for e in evs if e["Event"] == "EquipmentDisconnected"]
    expect(len(disc) == 1 and disc[0].get("reconnecting") is True, f"EquipmentDisconnected: {names}")
    want = "After 5.5 sec the camera has not completed a 0.5 sec exposure"
    expect(str(disc[0].get("reason")).startswith(want), f"reason {disc[0].get('reason')!r}")
    lost = [e for e in evs if e["Event"] == "StarLost"]
    expect(not lost, f"{len(lost)} StarLost for a capture timeout, expected none: {names}")
    expect(cli.call("get_app_state") == "Guiding", "not guiding after the reconnect")
    seq = [n for n in names if n in ("StarLost", "EquipmentDisconnected", "EquipmentReconnected")]
    expect(seq == ["EquipmentDisconnected", "EquipmentReconnected"], f"order {seq}")
    return f"{seq}, {len(lost)} StarLost, guiding (events {names})"


def guide_after_fake_back(lane):
    """Guide (no recalibration) on the fake back on the same port: it settles, with the side-of-pier Alert."""
    cli = lane.cli
    start = cli.mark()
    expect(cli.call("guide", {"settle": e2e.SETTLE, "recalibrate": False}) == 0, "guide did not return 0")
    done = cli.wait_events(start, "SettleDone", timeout=e2e.SETTLE["timeout"] + 60, abort_on=GUIDE_FAILURES)[0]
    expect(done.get("Status") == 0, f"SettleDone {strip(done)}")
    resumed = cli.events_since(start)
    expect(not [e for e in resumed if e["Event"] == "StartCalibration"], "guide recalibrated after the server came "
                                                                         "back")
    # the calibration was made with sideofpier failing: guiding on it warns
    expect(alerts(resumed, "Current calibration did not have side-of-pier information"),
           f"no side-of-pier Alert guiding on a calibration without it: {names_of(resumed)}")
    cli.wait_events(start, "GuideStep", count=3, timeout=60, abort_on=GUIDE_FAILURES)


def scenario_fake_killed(lane):
    """The Alpaca server gone between two camera calls while guiding, then back on the same port (#105, #190)."""
    cli = lane.cli
    expect(cli.call("get_app_state") == "Guiding", "not guiding")
    truth = lane.fake.truth()["field_offset_px"]
    server = f"{lane.fake.host}:{lane.fake.port}"
    mark = cli.mark()
    # no call cut: the one the fake holds is answered and the next one is refused (#211; a kill that lands during
    # a call is scenario_fake_killed_mid_request)
    refuse_fake_between_calls(lane)
    try:
        cli.wait_events(mark, "GuidingStopped", timeout=90)
    finally:
        lane.stop_fake()  # the held call was answered before the refused one, so this cuts nothing
    evs = settle_events(cli, mark, quiet_s=2.0)
    names = names_of(evs)
    state = cli.call("get_app_state")
    conn = connected_by_device(cli)
    stopped = [e for e in evs if e["Event"] in ("GuidingStopped", "LoopingExposuresStopped")]
    expect(state == "Stopped", f"app state {state} with the server gone")
    # the mount is not changed by #190: it still reads connected
    expect(conn == {"all": False, "camera": False, "mount": True, "rotator": False},
           f"get_connected with the server gone: {conn}")
    # #190: the refused connection goes through the camera reconnect path, which fails with the server gone
    equip = [e for e in evs if e["Event"].startswith("Equipment")]
    expect([e["Event"] for e in equip] == ["EquipmentDisconnected", "EquipmentReconnectFailed"],
           f"Equipment* events with the server gone: {names}")
    disc, failed = equip
    t_failed = failed["_t"]
    want = f"Cannot connect to the Alpaca server at {server} (refused or unreachable)"
    expect(disc.get("device_type") == "camera" and disc.get("reconnecting") is True and disc.get("reason") == want,
           f"EquipmentDisconnected {strip(disc)}")
    expect(failed.get("device_type") == "camera" and failed.get("reason") == "the camera did not reconnect",
           f"EquipmentReconnectFailed {strip(failed)}")
    lost = [e for e in evs if e["Event"] == "StarLost"]
    seq = [n for n in names if n != "ConfigurationChange"]
    # the camera's Alert, then the reconnect's own connect failure Alert ("Cannot reach server")
    want_seq = ["EquipmentDisconnected", "Alert", "Alert", "EquipmentReconnectFailed", "GuidingStopped",
                "LoopingExposuresStopped", "LockPositionLost"]  # one LoopingExposuresStopped (#186)
    expect(seq == want_seq, f"events with the server gone: {seq}, expected {want_seq}")
    camera_alerts = [a.get("Msg", "") for a in alerts(evs) if a.get("Msg", "").startswith(want)]
    expect(camera_alerts, f"no camera Alert {want!r}: {[a.get('Msg') for a in alerts(evs)]}")
    alert = camera_alerts[0]

    # back: the same field, so the star is where the lock is; the #104 retry reconnects the camera
    lane.start_fake()
    lane.fake.request("PUT", "/fake/shift", {"dx": truth["x"], "dy": truth["y"], "absolute": "true"})
    rec = cli.wait_events(mark, "EquipmentReconnected", timeout=RETRY_FIRST_S + RETRY_SECOND_S + 45)[0]
    t_rec = time.time()
    expect(rec.get("device_type") == "camera", f"EquipmentReconnected {strip(rec)}")
    conn_back = connected_by_device(cli)
    expect(conn_back == {"all": True, "camera": True, "mount": True, "rotator": False},
           f"get_connected after the retry: {conn_back}")
    expect(cli.call("get_app_state") == "Stopped", "the retry restarted capture")
    guide_after_fake_back(lane)
    return (f"server gone: {seq} ({len(lost)} StarLost; Alert {alert[:60]!r}), state {state}, get_connected "
            f"{conn}; back: the #104 retry reconnected the camera {t_rec - t_failed:.0f} s after "
            f"EquipmentReconnectFailed, guide settled without recalibrating, with the side-of-pier Alert "
            f"({len(stopped)} stop events)")


def scenario_fake_killed_mid_request(lane):
    """The Alpaca server killed while the guider waits on a startexposure, then back on the same port (#211, #237)."""
    cli = lane.cli
    expect(cli.call("get_app_state") == "Guiding", "not guiding")
    # the reconnect is the 4th within 60 s after the scenarios before it, which the throttle would refuse instead
    lane.wait_reconnect_window()
    expect(cli.call("get_app_state") == "Guiding", "not guiding after the wait")
    truth = lane.fake.truth()["field_offset_px"]
    server = f"{lane.fake.host}:{lane.fake.port}"
    mark = cli.mark()
    kill_fake_during_startexposure(lane)
    cli.wait_events(mark, "GuidingStopped", timeout=90)
    evs = settle_events(cli, mark, quiet_s=2.0)
    names = names_of(evs)
    state = cli.call("get_app_state")
    conn = connected_by_device(cli)
    expect(state == "Stopped", f"app state {state} with the server gone")
    # #237: the call the server died during fails with "no response from the Alpaca server" (curl's
    # CURLE_GOT_NOTHING, not a refused connection), which is transient like a timeout (#227): its frame is dropped
    # (StarLost with the reason) and guiding goes on. The next frame's call is refused, so it goes through the #190
    # camera reconnect path, which fails with the server gone, and guiding stops
    lost = [e for e in evs if e["Event"] == "StarLost"]
    expect(len(lost) == 1, f"{len(lost)} StarLost for the frame the kill cut, expected 1: {names}")
    status = str(lost[0].get("Status", ""))
    expect(status.startswith("camera error: no response from the Alpaca server"), f"StarLost Status {status!r}")
    equip = [e for e in evs if e["Event"].startswith("Equipment")]
    expect([e["Event"] for e in equip] == ["EquipmentDisconnected", "EquipmentReconnectFailed"],
           f"Equipment* events with the server gone: {names}")
    disc, failed = equip
    t_failed = failed["_t"]
    want = f"Cannot connect to the Alpaca server at {server} (refused or unreachable)"
    expect(disc.get("device_type") == "camera" and disc.get("reconnecting") is True and disc.get("reason") == want,
           f"EquipmentDisconnected {strip(disc)}")
    expect(failed.get("device_type") == "camera" and failed.get("reason") == "the camera did not reconnect",
           f"EquipmentReconnectFailed {strip(failed)}")
    # the Alerts go out from the camera's thread, so only their texts are checked, not where they fall
    seq = [n for n in names if n not in ("ConfigurationChange", "Alert")]
    want_seq = ["StarLost", "EquipmentDisconnected", "EquipmentReconnectFailed", "GuidingStopped",
                "LoopingExposuresStopped", "LockPositionLost"]  # one LoopingExposuresStopped (#186)
    expect(seq == want_seq, f"events with the server gone: {seq}, expected {want_seq} (all: {names})")
    msgs = [a.get("Msg", "") for a in alerts(evs)]
    # the kill lands in the stalled startexposure, so that is the call that fails (#211)
    cut = [m for m in msgs if m.startswith("Alpaca error -- Cannot start exposure: no response from the Alpaca server")]
    expect(len(cut) == 1, f"no single Cannot start exposure: no response Alert: {msgs}")
    expect([m for m in msgs if m.startswith(want)], f"no camera Alert {want!r}: {msgs}")
    # the mount is not changed by #190: it still reads connected
    expect(conn == {"all": False, "camera": False, "mount": True, "rotator": False},
           f"get_connected with the server gone: {conn}")

    # back: the same field, so the star is where the lock is; the #104 retry reconnects the camera
    lane.start_fake()
    lane.fake.request("PUT", "/fake/shift", {"dx": truth["x"], "dy": truth["y"], "absolute": "true"})
    rec = cli.wait_events(mark, "EquipmentReconnected", timeout=RETRY_FIRST_S + RETRY_SECOND_S + 45)[0]
    t_rec = time.time()
    expect(rec.get("device_type") == "camera", f"EquipmentReconnected {strip(rec)}")
    conn_back = connected_by_device(cli)
    expect(conn_back == {"all": True, "camera": True, "mount": True, "rotator": False},
           f"get_connected after the retry: {conn_back}")
    expect(cli.call("get_app_state") == "Stopped", "the retry restarted capture")
    guide_after_fake_back(lane)
    return (f"server gone: {seq} (Alert {cut[0][:60]!r}, StarLost {status[:50]!r}), state {state}, get_connected "
            f"{conn}; back: the #104 retry reconnected the camera {t_rec - t_failed:.0f} s after "
            f"EquipmentReconnectFailed, guide settled without recalibrating")


def scenario_guiding_assistant(lane):
    """GA start/stop while guiding, what it turns off and puts back, apply; and a run started from looping."""
    cli = lane.cli
    out = []
    expect(cli.call("get_app_state") == "Guiding", "not guiding")
    cli.call("set_variable_delay_settings", {"Enabled": True, "ShortDelaySeconds": 1, "LongDelaySeconds": 2})
    before = {"output": cli.call("get_guide_output_enabled"), "vardelay": cli.call("get_variable_delay_settings"),
              "multistar": setup(cli)["multistar_enabled"], "subframes": cli.call("get_use_subframes")}
    expect(before["output"] is True and before["vardelay"]["Enabled"] is True and before["multistar"] is True,
           f"before the run: {before}")
    lock_before = cli.call("get_lock_position")
    expect(call_error(cli, "guiding_assistant_apply", {"which": ["min_move_ra"]})[0] == 1,
           "guiding_assistant_apply before any run did not fail")
    mark = cli.mark()
    st = cli.call("guiding_assistant_start", {"duration_s": GA_DURATION_S, "measure_backlash": False})
    expect(st.get("state") == "measuring", f"guiding_assistant_start returned {st}")
    during = {"output": cli.call("get_guide_output_enabled"), "vardelay": cli.call("get_variable_delay_settings"),
              "multistar": setup(cli)["multistar_enabled"]}
    expect(during["output"] is False and during["vardelay"]["Enabled"] is False and during["multistar"] is False,
           f"during the run: {during}")
    expect_error(cli, "guide", {"settle": e2e.SETTLE}, 1,
                 "Guiding Assistant is running - call guiding_assistant_stop first")
    expect_error(cli, "guiding_assistant_start", {"duration_s": GA_DURATION_S, "measure_backlash": False}, 1,
                 "the Guiding Assistant is already running - call guiding_assistant_stop first")
    expect_error(cli, "guiding_assistant_apply", {"which": ["min_move_ra"]}, 1, "the Guiding Assistant is still "
                                                                                 "running")
    # past MAX_FAILED_GUIDE_MOVES frames: the run's guide output is off, and its refused moves are not failed moves
    # that stop guiding (#213)
    cli.wait_events(mark, "GuidingAssistantProgress", count=MAX_FAILED_GUIDE_MOVES + 3, timeout=90,
                    abort_on=("GuidingAssistantDone", "GuidingStopped"))
    expect(not alerts(cli.events_since(mark), FAILED_MOVES_ALERT), "the failed-moves Alert during the run")
    st = cli.call("guiding_assistant_stop")
    expect(st.get("state") == "done", f"guiding_assistant_stop returned {st}")
    done = cli.wait_events(mark, "GuidingAssistantDone", timeout=30)[0]
    expect(done.get("state") == "done" and "recommendations" in done, f"GuidingAssistantDone {strip(done)}")
    recs = st["recommendations"]
    # Restoring multi-star keeps guiding on the star and lock position of the run (#187): until then
    # GuiderMultiStar::SetMultiStarMode stopped guiding, auto-selected a star (moving the lock to wherever the star
    # had drifted with guide output off) and started guiding again, with no StarSelected, LockPositionSet or settle.
    # The secondary stars are found again where they are once the primary star is back on the lock
    lane.debug.mark()
    after_done = cli.events_since(mark).index(done) + mark + 1
    cli.wait_events(after_done, "GuideStep", count=3, timeout=60, abort_on=GUIDE_FAILURES)
    seq = [n for n in names_of(cli.events_since(mark), skip=("LoopingExposures", "GuideStep", "Settling",
                                                               "GuidingAssistantProgress", "ConfigurationChange"))]
    want = ["GuideParamChange", "GuideParamChange", "GuideParamChange", "GuideParamChange", "GuidingAssistantDone"]
    expect(seq == want, f"events of the run and its stop: {seq}, expected {want} (#187)")
    params = [(e.get("Name"), e.get("Value")) for e in cli.events_since(mark) if e["Event"] == "GuideParamChange"]
    expect(params == [("MultiStar", "false"), ("MountGuidingEnabled", False), ("MountGuidingEnabled", True),
                      ("MultiStar", "true")], f"GuideParamChange events {params}")
    lock_after = cli.call("get_lock_position")
    expect(lock_after == lock_before, f"lock position {lock_before} before the run, {lock_after} after it (#187)")
    lane.debug.wait(r"MultiStar: updating star positions after lock position change", 90, daemon=lane.daemon)
    after = {"output": cli.call("get_guide_output_enabled"), "vardelay": cli.call("get_variable_delay_settings"),
             "multistar": setup(cli)["multistar_enabled"], "subframes": cli.call("get_use_subframes")}
    expect(after == before, f"after the run {after}, before {before}")
    expect(cli.call("get_app_state") == "Guiding", "not guiding after the run")
    out.append(f"while guiding: {st['samples']} samples, off during the run {during}, restored {after}; the stop: "
               f"{seq} (guiding went on, lock {lock_after} kept, secondary stars found again)")

    # apply: all or nothing, then min-move
    code, msg = call_error(cli, "guiding_assistant_apply", {"which": ["min_move_ra", "bogus"]})
    expect(code == 1 and msg.startswith("unknown recommendation 'bogus'"), f"apply bogus: {code} {msg!r}")
    code, msg = call_error(cli, "guiding_assistant_apply", {"which": ["dec_backlash"]})
    expect(code == 1, f"apply dec_backlash with no backlash measured: {code} {msg!r}")
    mm_before = min_moves(cli)
    # The response must be valid JSON, read on a connection of its own so a bad line cannot stop e2e.Client's
    # reader: until #184 the applied names came back unquoted ([min_move_ra,min_move_dec]) and no JSON client could
    # read the answer
    raw = raw_call(lane.args.rpc_port, "guiding_assistant_apply", {"which": ["min_move_dec", "min_move_ra"]})
    try:
        parsed = json.loads(raw)
    except ValueError:
        raise Failure(f"guiding_assistant_apply responded with invalid JSON: {raw!r} (#184)")
    expect(parsed.get("result") == {"applied": ["min_move_ra", "min_move_dec"]},
           f"guiding_assistant_apply responded {raw!r}, expected applied [min_move_ra, min_move_dec]")
    mm_after = min_moves(cli)
    expect(abs(mm_after[0] - recs["min_move_ra_px"]) < 0.006 and abs(mm_after[1] - recs["min_move_dec_px"]) < 0.006,
           f"MinMoves {mm_after} after apply, recommended {recs['min_move_ra_px']} / {recs['min_move_dec_px']}")
    out.append(f"apply: unknown name and dec_backlash refused ({msg!r}); min_move applied (response {raw!r}): "
               f"{mm_before} -> {mm_after}")

    # from looping on the calibration: the run starts guiding, and goes back to looping at the end
    cli.call("stop_capture")
    cli.wait_state("Stopped", 30)
    e2e.loop_and_find_star(cli)
    mark = cli.mark()
    st = cli.call("guiding_assistant_start", {"duration_s": GA_DURATION_S, "measure_backlash": False})
    cli.wait_events(mark, "GuidingAssistantProgress", count=4, timeout=90, abort_on=("GuidingAssistantDone",))
    evs = cli.events_since(mark)
    expect(not [e for e in evs if e["Event"] == "StartCalibration"], "a GA run from looping calibrated")
    state_during = cli.call("get_app_state")
    cli.call("guiding_assistant_stop")
    ga_done = cli.wait_events(mark, "GuidingAssistantDone", timeout=30)[0]
    after_done = cli.events_since(mark).index(ga_done) + mark + 1
    # back to looping, on a star selected again (multi-star restored): app state Selected, frames still coming
    cli.wait_events(after_done, "LoopingExposures", count=2, timeout=60, abort_on=("LoopingExposuresStopped",))
    state_after = cli.call("get_app_state")
    expect(state_during == "Guiding" and state_after == "Selected",
           f"app state {state_during} during a run from looping, {state_after} after it")
    expect(cli.call("get_guide_output_enabled") is True, "guide output still off after a run from looping")
    seq = [n for n in names_of(cli.events_since(mark)) if n in ("StartGuiding", "GuidingStopped", "SettleBegin",
                                                                  "GuidingAssistantDone", "LoopingExposuresStopped")]
    expect(seq == ["StartGuiding", "GuidingStopped", "GuidingAssistantDone"], f"events of a run from looping: {seq}")
    out.append(f"from looping: {state_during} during the run (no calibration, no settle), {state_after} and "
               f"looping after it ({seq})")
    cli.call("stop_capture")
    cli.wait_state("Stopped", 30)
    cli.call("set_variable_delay_settings", {"Enabled": False, "ShortDelaySeconds": 1, "LongDelaySeconds": 2})
    return "; ".join(out)


def scenario_reconnect_throttle(lane):
    """A 4th camera reconnect within 60 s: EquipmentReconnectFailed, then the #104 retry reconnects the camera."""
    cli, fake = lane.cli, lane.fake
    lane.wait_reconnect_window()
    start = cli.mark()
    cli.call("loop")
    cli.wait_events(start, "LoopingExposures", count=2, timeout=60, abort_on=("LoopingExposuresStopped",))
    t0 = time.time()
    for i in range(RECONNECT_MAX):
        mark = cli.mark()
        fake.inject("imagearray", 1031, 1, f"e2e throttle {i + 1}")
        rec = cli.wait_events(mark, "EquipmentReconnected", timeout=60, abort_on=("EquipmentReconnectFailed",))
        after = cli.events_since(mark).index(rec[0]) + mark + 1
        cli.wait_events(after, "LoopingExposures", count=1, timeout=60, abort_on=("LoopingExposuresStopped",))
    burst = time.time() - t0
    expect(burst < RECONNECT_WINDOW_S - 10, f"the {RECONNECT_MAX} reconnects took {burst:.0f} s")
    mark = cli.mark()
    fake.inject("imagearray", 1031, 1, "e2e throttle 4")
    failed = cli.wait_events(mark, "EquipmentReconnectFailed", timeout=60)[0]
    t_failed = time.time()
    want = f"auto-reconnect throttle exhausted ({RECONNECT_MAX} attempts in {RECONNECT_WINDOW_S} s)"
    expect(failed.get("device_type") == "camera" and failed.get("attempts") == RECONNECT_MAX and
           failed.get("reason") == want, f"EquipmentReconnectFailed {strip(failed)}")
    cli.wait_events(mark, "LoopingExposuresStopped", timeout=30)
    evs = settle_events(cli, mark)
    names = names_of(evs)
    expect(cli.call("get_app_state") == "Stopped", f"app state {cli.call('get_app_state')} after the throttle")
    expect(cli.call("get_connected", {"device": "camera"}) is False, "the camera reads connected after the throttle")
    seq = [n for n in names if n.startswith("Equipment") or n == "LoopingExposuresStopped"]
    want_seq = ["EquipmentDisconnected", "EquipmentReconnectFailed", "LoopingExposuresStopped"]  # one (#186)
    expect(seq == want_seq, f"events of the 4th reconnect: {seq}, expected {want_seq}")
    # the #104 retry takes over
    rec = cli.wait_events(mark, "EquipmentReconnected", timeout=RETRY_FIRST_S + 45)[0]
    t_rec = time.time()
    expect(rec.get("device_type") == "camera", f"EquipmentReconnected {strip(rec)}")
    expect(cli.call("get_connected") is True, "not connected after the retry")
    expect(cli.call("get_app_state") == "Stopped", "the retry restarted capture")
    expect(RETRY_FIRST_S - 3 <= t_rec - t_failed <= RETRY_FIRST_S + 30,
           f"the retry reconnected {t_rec - t_failed:.0f} s after EquipmentReconnectFailed")
    return (f"{RECONNECT_MAX} reconnects in {burst:.0f} s, the 4th: {seq}, {strip(failed)}; Stopped, camera "
            f"disconnected; the #104 retry reconnected it {t_rec - t_failed:.0f} s later, capture not restarted")


def scenario_camera_stale_connected(lane):
    """A camera the server reads as connected while every capture fails 1031 is not reported back: the reconnect
    waits for a frame, the failed one sends EquipmentReconnectFailed, and the #104 retry reconnects it once it is
    back (RC51 2026-11-22: AlpacaBridge read an ASI120 unbound from USB as connected)."""
    cli, fake = lane.cli, lane.fake
    lane.wait_reconnect_window()
    start = cli.mark()
    cli.call("loop")
    cli.wait_events(start, "LoopingExposures", count=2, timeout=60, abort_on=("LoopingExposuresStopped",))
    mark = cli.mark()
    fake.request("PUT", "/fake/unplug", {"unplugged": "true", "stale_connected": "true"})
    try:
        failed = cli.wait_events(mark, "EquipmentReconnectFailed", timeout=60, abort_on=("EquipmentReconnected",))[0]
        t_failed = time.time()
        cli.wait_events(mark, "LoopingExposuresStopped", timeout=30)
        evs = settle_events(cli, mark)
        seq = [n for n in names_of(evs) if n.startswith("Equipment") or n == "LoopingExposuresStopped"]
        want_seq = ["EquipmentDisconnected", "EquipmentDisconnected", "EquipmentReconnectFailed",
                    "LoopingExposuresStopped"]
        expect(seq == want_seq, f"events of a camera read as connected while removed: {seq}, expected {want_seq}")
        disc = [e for e in evs if e["Event"] == "EquipmentDisconnected"]
        expect(all(e.get("device_type") == "camera" and e.get("reconnecting") is True for e in disc),
               f"EquipmentDisconnected {[strip(e) for e in disc]}")
        want = "the camera failed its first frame after reconnecting"
        expect(failed.get("device_type") == "camera" and failed.get("reason") == want,
               f"EquipmentReconnectFailed {strip(failed)}")
        expect(cli.call("get_app_state") == "Stopped", f"app state {cli.call('get_app_state')}")
        expect(cli.call("get_connected", {"device": "camera"}) is False, "the camera reads connected while removed")
        time.sleep(3)
        late = [n for n in names_of(cli.events_since(mark)) if n == "EquipmentReconnected"]
        expect(not late, "EquipmentReconnected while the camera was still removed")
    finally:
        fake.request("PUT", "/fake/unplug", {"unplugged": "false"})
    # the #104 retry takes over
    rec = cli.wait_events(mark, "EquipmentReconnected", timeout=RETRY_FIRST_S + 45)[0]
    t_rec = time.time()
    expect(rec.get("device_type") == "camera", f"EquipmentReconnected {strip(rec)}")
    expect(cli.call("get_connected") is True, "not connected after the retry")
    expect(cli.call("get_app_state") == "Stopped", "the retry restarted capture")
    return (f"{seq}, {strip(failed)}; Stopped, camera disconnected; the #104 retry reconnected it "
            f"{t_rec - t_failed:.0f} s later")


def scenario_ga_exit_restore(lane):
    """shutdown while a GA run is measuring: the settings it turned off are on again after a restart."""
    cli = lane.cli
    cli.call("set_variable_delay_settings", {"Enabled": True, "ShortDelaySeconds": 1, "LongDelaySeconds": 2})
    before = {"vardelay": cli.call("get_variable_delay_settings"), "multistar": setup(cli)["multistar_enabled"]}
    e2e.loop_and_find_star(cli)
    mark = cli.mark()
    cli.call("guiding_assistant_start", {"duration_s": GA_DURATION_S, "measure_backlash": False})
    cli.wait_events(mark, "GuidingAssistantProgress", count=2, timeout=90, abort_on=("GuidingAssistantDone",))
    expect(setup(cli)["multistar_enabled"] is False, "multi-star still on during the run")
    lane.end_daemon()  # the shutdown RPC, mid-run
    # the lane's profile is still current in the config; no equipment, nothing connects
    cli = lane.start_daemon()
    after = {"vardelay": cli.call("get_variable_delay_settings"), "multistar": setup(cli)["multistar_enabled"]}
    expect(after == before, f"after the restart {after}, before the run {before}")
    cli.call("set_variable_delay_settings", {"Enabled": False, "ShortDelaySeconds": 1, "LongDelaySeconds": 2})
    return f"shutdown mid-run exits 0; after the restart {after} (as before the run)"


# ---------------------------------------------------------------------------------------------------------------


CONFIGURED_DEVICES_GET = "GET /management/v1/configureddevices"


def free_port():
    """A TCP port nothing listens on, from the kernel: the second fake of a scenario, off the lanes' own ports."""
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def fake_log_from(fake, offset):
    with open(fake.log.name, "rb") as f:
        f.seek(offset)
        return f.read().decode("utf-8", errors="replace")


def scenario_imaging_gear_server_change(lane):
    """#236: set_alpaca_server moves the imaging-gear reader to the new server at once, and get_imaging_gear never
    reports the server it left."""
    cli, a = lane.cli, lane.fake
    b = e2e.Fake(free_port(), lane.work, FAKE_ARGS, log_name="fake_alpaca-236.log")
    base = cli.call("get_profile")
    name = f"e2e {lane.name} 236 {int(time.time())}"
    cli.call("create_profile", {"name": name, "select": True})
    out = []

    def server_of(fake):
        return f"{fake.host}:{fake.port}"

    def gear_server():
        gear = cli.call("get_imaging_gear")
        return gear["source"], gear["server"]

    def wait_gear(fake, timeout=15):
        deadline = time.time() + timeout
        while True:
            source, server = gear_server()
            if source == "alpaca" and server.endswith(" " + server_of(fake)):
                return server
            expect(time.time() < deadline, f"get_imaging_gear {source} {server!r} within {timeout} s, expected the "
                                           f"server {server_of(fake)}")
            time.sleep(0.25)

    try:
        cli.call("set_alpaca_server", {"host": a.host, "port": a.port, "camera_device": 0, "telescope_device": 0})
        out.append(f"on {wait_gear(a)!r}")

        # nothing but the server change wakes the reader: no get_imaging_gear until the new server has been read
        at = os.path.getsize(b.log.name)
        cli.call("set_alpaca_server", {"port": b.port})
        t = time.time()
        while CONFIGURED_DEVICES_GET not in fake_log_from(b, at):
            expect(time.time() - t < 5, f"the imaging-gear reader did not read the new server {server_of(b)} within "
                                        "5 s of set_alpaca_server")
            time.sleep(0.1)
        out.append(f"set_alpaca_server {server_of(b)}: its configureddevices read after {time.time() - t:.1f} s")
        out.append(f"then {wait_gear(b)!r}")

        # straight after a change, the server left is never reported
        for fake, left in ((a, b), (b, a)):
            cli.call("set_alpaca_server", {"port": fake.port})
            source, server = gear_server()
            expect(source == "unavailable" or (source == "alpaca" and server.endswith(" " + server_of(fake))),
                   f"get_imaging_gear straight after set_alpaca_server {server_of(fake)}: {source} {server!r} "
                   f"(left {server_of(left)})")
            out.append(f"straight after the change to {server_of(fake)}: {source}")
            wait_gear(fake)
    finally:
        cli.call("set_profile", {"id": base["id"]})
        cli.call("delete_profile", {"name": name})
        b.stop()
    return "; ".join(out)


def run_gear(lane, args):
    lane.start_fake()
    lane.start_daemon()
    lane.step("set_alpaca_server moves the imaging-gear reader (#236)",
              lambda: scenario_imaging_gear_server_change(lane))
    if lane.step("connect side effects (set_connected, get_connected, #139 latch, refusals)",
                 lambda: scenario_connect_side_effects(lane)):
        if lane.step("camera change with the mount off (#139)", lambda: scenario_camera_change_mount_off(lane, args)):
            lane.step("image scale change: the guide speed sources (#115 N4)",
                      lambda: scenario_guide_speed_sources(lane))
            lane.step("binning change on guide start (#115 N4)", lambda: scenario_binning_change(lane, args))
    lane.step("startup --auto-connect against the running fake", lambda: scenario_auto_connect(lane))
    lane.step("startup --auto-connect, the fake starting late (#104 retry)",
              lambda: scenario_auto_connect_late_fake(lane))
    lane.step("stale and case-changed selections in the profile file, restarted",
              lambda: scenario_stale_selection(lane))
    lane.step("guide connects the camera and mount first (#104)", lambda: scenario_guide_connects_first(lane, args))
    lane.step("set_alpaca_server keeps the selections and the #104 retry", lambda: scenario_alpaca_server_carry(lane))
    lane.step("shutdown during a #104 probe", lambda: scenario_shutdown_during_probe(lane))


def run_faults(lane, args):
    lane.start_fake()
    cli = lane.start_daemon()

    def connect():
        # multi-star on, so the Guiding Assistant has it to turn off (a new profile has it off: the guider loads it
        # with default false, while get_profile_setup's default is true, and writes it back)
        got = lane.make_profile(FAKE_PIXEL_UM, dict(QUICK_CALIBRATION, multistar_enabled=True))
        expect(got["multistar_enabled"] is True and got["auto_restore_calibration"] is False,
               f"the new profile's setup: {got}")
        cli.call("set_connected", {"connected": True}, timeout=60)
        expect(cli.call("get_connected") is True, "the equipment did not connect")
        return f"profile {lane.profile[1]}"

    def fake_killed(scenario):
        def run():
            try:
                return scenario(lane)
            finally:
                if lane.fake.proc.poll() is not None:  # failed with the server gone: the steps after it need one (#211)
                    lane.start_fake()
        return run

    if not lane.step("connect the fake camera and mount", connect):
        return
    lane.step("camera drop while looping (1280, 1031)", lambda: scenario_camera_drop_loop(lane))
    calibrated = lane.step("mount declination/sideofpier errors during calibration",
                           lambda: scenario_mount_read_errors_calibration(lane, args))
    # each from a known start once a step has failed (#233): guiding on the kept calibration, or stopped
    for name, fn, guiding in (
            ("pulseguide errors while guiding", lambda: scenario_pulseguide_errors(lane), True),
            ("guide output off while guiding (#213)", lambda: scenario_guide_output_off(lane), True),
            ("3rd camera drop in a row while guiding (#103 reconnect)", lambda: scenario_third_drop(lane), True),
            ("capture timeout while guiding", lambda: scenario_capture_timeout(lane), True),
            ("Alpaca server gone between calls and back while guiding (#105, #190)",
             fake_killed(scenario_fake_killed), True),
            ("Alpaca server killed during a startexposure and back while guiding (#211, #237)",
             fake_killed(scenario_fake_killed_mid_request), True),
            ("Guiding Assistant start/stop/apply and restore", lambda: scenario_guiding_assistant(lane), True),
            ("camera reconnect throttle, then the #104 retry", lambda: scenario_reconnect_throttle(lane), False),
            ("camera read as connected while removed: no EquipmentReconnected without a frame",
             lambda: scenario_camera_stale_connected(lane), False),
            ("Guiding Assistant settings restored on exit", lambda: scenario_ga_exit_restore(lane), False)):
        if calibrated:
            lane.step_from_known_start(name, fn, guiding)
        else:
            lane.skip(name, "no calibration: the calibration step failed")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--lane", choices=("gear", "faults"), required=True)
    ap.add_argument("--phd2", required=True, help="the phd2.bin to run (an ASan/UBSan build in CI)")
    ap.add_argument("--instance", type=int, required=True, help="daemon instance (-i); JSON-RPC on 4400 + N - 1")
    ap.add_argument("--fake-port", type=int, required=True, help="the fake Alpaca server's port")
    ap.add_argument("--display", required=True, help="X display number for xvfb-run, or auto (xvfb-run -a)")
    ap.add_argument("--auto-display-from", type=int, default=99,
                    help="with --display auto, the first display number xvfb-run -a tries")
    ap.add_argument("--work-dir", required=True, help="the scratch HOME and every log go here (emptied first)")
    ap.add_argument("--timeout", type=float, default=2000, help="the whole lane, seconds")
    ap.add_argument("--calibration-timeout", type=float, default=600, help="seconds to CalibrationComplete")
    args = ap.parse_args()
    args.rpc_port = 4400 + args.instance - 1

    work = os.path.abspath(args.work_dir)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    t0 = time.time()

    def on_alarm(signum, frame):
        raise e2e.RunTimeout(f"the lane took longer than {args.timeout:.0f} s")

    def on_term(signum, frame):
        e2e.TERMINATED.append(signum)
        raise e2e.RunTimeout("terminated")

    signal.signal(signal.SIGALRM, on_alarm)
    signal.signal(signal.SIGTERM, on_term)
    signal.alarm(int(args.timeout))

    lane = Lane(args.lane, args, work)
    try:
        (run_gear if args.lane == "gear" else run_faults)(lane, args)
    except (Failure, e2e.RunTimeout) as e:
        lane.failures.append(("run", str(e)))
        print(f"FAIL run: {e}", flush=True)
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGTERM, lambda signum, frame: e2e.TERMINATED.append(signum))
        try:
            if lane.cli is not None and lane.daemon is not None and not lane.daemon.exited():
                if lane.profile is not None:
                    lane.step("disconnect and restore the profile", lane.restore_profile)
                lane.step("shutdown", lane.end_daemon)
        finally:
            if lane.cli is not None:
                lane.cli.close()
            if lane.daemon is not None:
                lane.daemon.kill()
                status = lane.daemon.proc.returncode
                if status not in (0, None) and not any(n == "shutdown" for n, _ in lane.failures):
                    lane.failures.append(("daemon exit", f"the daemon exited with status {status}"))
                lane.check_sanitizers(lane.daemon)
                e2e.collect_logs(lane.daemon, work)
            lane.stop_fake()
        if e2e.TERMINATED and not any(n == "run" for n, _ in lane.failures):
            lane.failures.append(("run", "terminated"))

    print("")
    print(f"e2e {args.lane}: {len(lane.results)} passed, {len(lane.failures)} failed, {len(lane.skipped)} skipped, "
          f"{time.time() - t0:.0f} s; logs in {work}")
    for name, err in lane.failures:
        print(f"- {name}: {err}")
    for name, why in lane.skipped:
        print(f"- skipped {name}: {why}")
    return 1 if lane.failures else 0


if __name__ == "__main__":
    sys.exit(main())
