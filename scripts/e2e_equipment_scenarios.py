#!/usr/bin/env python3
"""Equipment and fault scenarios of the daemon end-to-end run (#142 Tier 1).

Characterization: each scenario pins what the daemon does today, at the JSON-RPC and event level Ara sees, as the
safety net for #115 step 3 (connect, disconnect, auto-connect, the #104 connect retry and the #105 camera reconnect
moving out of GearDialog). A check that fails is a change of behaviour, not necessarily a bug; the bugs found while
writing these are pinned as they are and reported, not fixed here.

Two lanes, run by scripts/e2e_guide_scenarios.py beside its main and physics lanes (each on its own instance, fake
port and display; see that script's LaneChild), or alone with --lane:

the gear lane, on a fake that answers ImageBytes (no calibration except the one the camera-change scenario needs):

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
  auto-connect            the daemon restarted with --auto-connect (as debian/openastro-guider.service runs it)
                          against the running fake: connected by the time the RPC port answers, the saved
                          calibration auto-loaded, no EquipmentReconnected
  auto-connect, late      restarted with --auto-connect and no fake: not connected; the #104 retry 15 s and then
  fake (#104)             30 s after each failure, logged, with no event and no Alert; the fake started after the
                          second failure: EquipmentReconnected camera and mount at the third attempt, connected

the faults lane, on a fake that answers ImageBytes, with one calibration (a long calibration step, so it is quick):

  camera drop mid-loop    one Alpaca 1280 while looping stops the loop (one LoopingExposuresStopped, #186); a 1031
                          reconnects the camera (EquipmentDisconnected reconnecting, EquipmentReconnected) and the
                          loop goes on
  mount read errors       declination and sideofpier failing through the whole calibration: the calibration
  during calibration      completes, get_calibration_data reporting the unknown Dec and pierSide as null (#189);
                          the profile's first calibration turns auto_restore_calibration on
  pulseguide errors       pulseguide failing while guiding: the first failed pulse raises an Alert, and then at most
                          one every 30 s (#188), not one per failed pulse; guiding goes on, the mount stays connected
  3rd camera drop         3 frames in a row failing with 1280 while guiding: 2 StarLost, then the #103 reconnect
                          (EquipmentDisconnected reconnecting, EquipmentReconnected) and guiding goes on
  capture timeout         an exposure that never completes (/fake/hold): the camera watchdog disconnects and
                          reconnects the camera, and guiding goes on
  fake killed (#105)      the Alpaca server gone mid-guide: guiding stops at the first failed frame, with no
                          reconnect, the camera and mount still reading connected; back on the same port, guide (no
                          recalibration) settles. The kill lands while a startexposure waits on the fake (#211)
  Guiding Assistant       start while guiding: guide output, multi-star and variable delay off during the run;
                          stopped after a few samples: restored, guiding going on with the same star and lock
                          position and the secondary stars found again (#187); apply's refusals, and min-move
                          applied with a valid JSON response (#184); a run started from looping guides and ends
                          looping
  reconnect throttle      a 4th camera reconnect within 60 s: EquipmentReconnectFailed, then the #104 retry
  (#105 -> #104)          reconnects it (EquipmentReconnected) 15 s later, capture not restarted
  GA exit restore         shutdown during a run: the multi-star and variable-delay settings the run turned off read
                          as before it after a restart

A lane fails on a failed check, a daemon exit before its shutdown or with a non-zero status, and any sanitizer
report. Everything it collects (each daemon's output and debug log, each fake's log, every event) is left in its
--work-dir.

    python3 scripts/e2e_equipment_scenarios.py --lane gear --phd2 build-asan/phd2.bin --instance 12 \\
        --fake-port 11152 --display 112 --work-dir /tmp/e2e/gear
"""
import argparse
import glob
import json
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
    axes to (AdvancedDialog::MakeImageScaleAdjustments); 0.2316 px for the fake's 3.75 um at 400 mm."""
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


def kill_fake_during_startexposure(lane, timeout=30):
    """Stop the fake while the guider waits on a startexposure, the kill a loaded machine can give (#211)."""
    at = fake_log_size(lane)
    lane.fake.request("PUT", "/fake/stall", {"device": "camera", "ms": 60000, "call": "startexposure"})
    deadline = time.time() + timeout
    while "stalling camera 60000 ms: /api/v1/camera/0/startexposure" not in fake_log_since(lane, at):
        expect(time.time() < deadline, f"no startexposure reached the fake within {timeout} s")
        time.sleep(0.1)
    lane.stop_fake()


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
        self.results, self.failures = [], []

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
    expect(cal_step1 != cal_step0, f"calibration_duration {cal_step1} not recomputed for the new image scale")
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

    # create_profile selects the new profile unless select is false: while connected it creates the profile, then
    # fails with set_profile's refusal, and the profile is left there
    other_name = f"e2e gear other {int(time.time())}"
    expect_error(cli, "create_profile", {"name": other_name}, 1, "cannot set profile when equipment is connected")
    made = [p for p in cli.call("get_profiles") if p.get("name") == other_name]
    expect(len(made) == 1 and not made[0].get("selected"), f"the profile create_profile refused: {made}")
    other_id = made[0]["id"]
    expect(cli.call("get_profile")["name"] != other_name, "create_profile switched the profile while connected")
    expect_error(cli, "create_profile", {"name": other_name, "select": False}, -32602, "profile name already exists")
    out.append("create_profile while connected: created, then refused (\"cannot set profile when equipment is "
               "connected\"), the profile left in place")

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
    """pulseguide failing while guiding: one Alert, at most one every 30 s (#188), guiding goes on, the mount stays
    connected."""
    cli, fake = lane.cli, lane.fake
    expect(cli.call("get_app_state") == "Guiding", "not guiding")
    # bigger corrections, so every frame sends a pulse
    fake.request("PUT", "/fake/shift", {"dx": 4, "dy": 4})
    mark = cli.mark()
    log0 = fake_log_size(lane)
    fake.request("PUT", "/fake/inject", {"device": "telescope", "call": "pulseguide", "error": 1280, "count": 100000,
                                         "message": "e2e pulseguide failed"})
    steps = cli.wait_events(mark, "GuideStep", count=6, timeout=90, abort_on=GUIDE_FAILURES)
    fake.request("PUT", "/fake/inject", {"device": "telescope", "call": "pulseguide", "count": 0})
    evs = settle_events(cli, mark)
    failed = fake_log_since(lane, log0).count("injected error 1280 on /api/v1/telescope/0/pulseguide")
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
    return (f"{len(steps)} GuideSteps with pulseguide failing, {failed} failed pulses, {len(pulse_alerts)} Alerts "
            f"{pulse_alerts[0].get('Msg')!r}, guiding, the mount connected; {len(pulses)} pulses after "
            f"(before the clear {pulses0})")


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


def scenario_fake_killed(lane):
    """The Alpaca server gone while guiding, then back on the same port (#105, #190)."""
    cli = lane.cli
    expect(cli.call("get_app_state") == "Guiding", "not guiding")
    truth = lane.fake.truth()["field_offset_px"]
    server = f"{lane.fake.host}:{lane.fake.port}"
    mark = cli.mark()
    kill_fake_during_startexposure(lane)
    cli.wait_events(mark, "GuidingStopped", timeout=90)
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
    return (f"server gone: {seq} ({len(lost)} StarLost; Alert {alert[:60]!r}), state {state}, get_connected "
            f"{conn}; back: the #104 retry reconnected the camera {t_rec - t_failed:.0f} s after "
            f"EquipmentReconnectFailed, guide settled without recalibrating, with the side-of-pier Alert "
            f"({len(stopped)} stop events)")


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
    cli.wait_events(mark, "GuidingAssistantProgress", count=6, timeout=60, abort_on=("GuidingAssistantDone",))
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


def run_gear(lane, args):
    lane.start_fake()
    lane.start_daemon()
    if lane.step("connect side effects (set_connected, get_connected, #139 latch, refusals)",
                 lambda: scenario_connect_side_effects(lane)):
        lane.step("camera change with the mount off (#139)", lambda: scenario_camera_change_mount_off(lane, args))
    lane.step("startup --auto-connect against the running fake", lambda: scenario_auto_connect(lane))
    lane.step("startup --auto-connect, the fake starting late (#104 retry)",
              lambda: scenario_auto_connect_late_fake(lane))


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

    def fake_killed():
        try:
            return scenario_fake_killed(lane)
        finally:
            if lane.fake.proc.poll() is not None:  # failed with the server gone: the steps after it need one (#211)
                lane.start_fake()

    if not lane.step("connect the fake camera and mount", connect):
        return
    lane.step("camera drop while looping (1280, 1031)", lambda: scenario_camera_drop_loop(lane))
    if lane.step("mount declination/sideofpier errors during calibration",
                 lambda: scenario_mount_read_errors_calibration(lane, args)):
        lane.step("pulseguide errors while guiding", lambda: scenario_pulseguide_errors(lane))
        lane.step("3rd camera drop in a row while guiding (#103 reconnect)", lambda: scenario_third_drop(lane))
        lane.step("capture timeout while guiding", lambda: scenario_capture_timeout(lane))
        if lane.step("Alpaca server killed and back while guiding (#105)", fake_killed):
            lane.step("Guiding Assistant start/stop/apply and restore", lambda: scenario_guiding_assistant(lane))
        lane.step("camera reconnect throttle, then the #104 retry", lambda: scenario_reconnect_throttle(lane))
        lane.step("Guiding Assistant settings restored on exit", lambda: scenario_ga_exit_restore(lane))


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
    print(f"e2e {args.lane}: {len(lane.results)} passed, {len(lane.failures)} failed, {time.time() - t0:.0f} s; "
          f"logs in {work}")
    for name, err in lane.failures:
        print(f"- {name}: {err}")
    return 1 if lane.failures else 0


if __name__ == "__main__":
    sys.exit(main())
