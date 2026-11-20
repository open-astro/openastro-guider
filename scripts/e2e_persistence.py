#!/usr/bin/env python3
"""Settings persistence across a daemon stop (#142 Tier 1).

RC51 gets its power cut in the field, so a setting a client changed over JSON-RPC has to survive a clean stop
(SIGTERM, which is what `systemctl stop` sends) and a hard kill (SIGKILL, the stand-in for a power cut). For each
method that changes a persistent setting this sets a distinctive non-default value and reads it back, stops the
daemon, starts it again on the same scratch HOME and instance, and reads every value back:

  SIGTERM pass   the stop is SIGTERM to phd2.bin, the graceful shutdown path
  SIGKILL pass   the stop is SIGKILL to phd2.bin: what is not on disk by then is lost

The two passes run at once, each with its own daemon instance, fake Alpaca port and X display (the given ones and
the next ones up). The first daemon of each pass runs under strace, which records how the config file is written:
every write must leave the file whole after a power cut at any point, so the new file has to be fsynced before it is
renamed over the old one, the old one must never be deleted first, and the directory has to be fsynced after the
rename (see check_write_protocol). No calibration is needed, so a pass takes a minute or so.

What is checked, by stage:

  before connecting  create_profile, clone_profile, rename_profile, delete_profile, set_profile_by_name and
                     set_profile; set_alpaca_server, set_selected_camera, set_selected_mount, set_camera_bitdepth,
                     set_profile_setup, set_dark_auto_load, set_dither_settings, set_star_detection,
                     set_auto_exposure, set_noise_reduction, set_guider_options, set_variable_delay_settings
  connected          set_algo, set_algo_param, set_exposure, set_dec_guide_mode, set_guide_limits, set_dec_comp,
                     set_mount_options, set_backlash_comp, set_camera_gain, set_camera_timeout, set_use_subframes,
                     set_camera_saturation

Left out, and why:

  session state, not settings    set_paused, set_lock_position, set_lock_shift_enabled, set_lock_shift_params,
                                 set_guide_output_enabled, set_pa_session, set_imaging_gear_devices,
                                 set_connected, set_cooler_state
  the fake has no such hardware  set_cooler_setpoint (no cooler), set_limit_frame (no frame limiting),
                                 set_selected_rotator and set_rotator_reversed (no rotator),
                                 set_selected_camera_id (Alpaca cameras have no camera ids)
  refused in this build          set_selected_aux_mount, set_selected_ao
  need a calibration, a Guiding  clear_calibration, flip_calibration, restore_calibration,
  Assistant run or dark frames   guiding_assistant_apply, set_dark_library_enabled, set_defect_map_enabled,
                                 build_dark_library, build_defect_map_darks, rebuild_defect_map, add_bad_pixel,
                                 delete_calibration_files

Each setting that does not survive is reported with its method name, and the run fails; so does a daemon that
crashes, exits non-zero after SIGTERM or `shutdown`, or prints a sanitizer report. Everything (each daemon's output and
debug log, the fakes' logs, the strace output) is left in --work-dir.

    python3 scripts/e2e_persistence.py --phd2 build-asan/phd2.bin --work-dir /tmp/e2e-persistence
"""
import argparse
import copy
import os
import re
import shutil
import signal
import sys
import threading
import time
import traceback

import e2e_guide_scenarios as e2e

expect = e2e.expect
Failure = e2e.Failure

MAIN = "e2e persist"
OTHER = "e2e persist other"
RENAMED = "e2e persist renamed"
CLONE = "e2e persist clone"
DOOMED = "e2e persist doomed"

# the syscalls strace records: the config write, and anything that deletes or renames a file
STRACE_SYSCALLS = "fsync,fdatasync,rename,renameat,renameat2,unlink,unlinkat"


class Setting:
    """One persistent setting: `apply` sets it over RPC, `read` returns {field: value} and `want` is what it must be.
    `prepare`, when given, runs first: what it changes is not counted as `apply`'s change on disk."""

    def __init__(self, method, apply, read, want, prepare=None):
        self.method, self.apply, self.read, self.want, self.prepare = method, apply, read, want, prepare


def rpc(method, params=None):
    return lambda cli: cli.call(method, params)


def fields(method, params=None, keys=None):
    """A read of `method`'s result: the whole value under "value", or only `keys` of an object result."""

    def read(cli):
        res = cli.call(method, params)
        if keys is None:
            return {"value": res}
        return {k: res.get(k) for k in keys}

    return read


def profile_names(cli):
    return [p["name"] for p in cli.call("get_profiles")]


def same(got, want):
    if isinstance(want, bool) or isinstance(got, bool):
        return got is want
    if isinstance(want, float) and isinstance(got, (int, float)):
        return abs(got - want) <= 1e-6 * max(1.0, abs(want))
    return got == want


def disconnected_settings(fake):
    """Set before the equipment is connected (profile and equipment selection must be, and the rest can be)."""
    cam = f"Alpaca Camera [{fake.host}:{fake.port}/0]"
    mount = f"Alpaca Mount [{fake.host}:{fake.port}/0]"

    def create_main(cli):
        cli.call("create_profile", {"name": MAIN, "select": True})
        cli.call("create_profile", {"name": OTHER, "select": False})

    def select_other(cli):
        # select a profile other than MAIN by name, so that set_profile below has something to change
        cli.call("set_profile_by_name", {"name": RENAMED})
        expect(cli.call("get_profile")["name"] == RENAMED, "set_profile_by_name did not select the profile")

    def select_main(cli):
        main_id = next(p["id"] for p in cli.call("get_profiles") if p["name"] == MAIN)
        cli.call("set_profile", {"id": main_id})

    star = {"MinStarSNR": 9.5, "MinStarHFD": 1.75, "MaxStarHFD": 8.25, "SearchRegion": 23, "MassChangeThreshold": 0.65}
    setup = {"focal_length": 777, "pixel_size": 3.75, "guide_speed": 0.75, "calibration_duration": 1234,
             "calibration_distance": 33, "high_res_encoders": True, "auto_restore_calibration": True,
             "multistar_enabled": True, "mass_change_threshold_enabled": False}
    alpaca = {"host": fake.host, "port": fake.port, "camera_device": 0, "telescope_device": 0, "rotator_device": 3}
    guider = {"FastRecenter": False, "AutoSelDownsample": 3, "TolerateJumps": True, "TolerateJumpsThreshold": 7.5}
    delay = {"Enabled": True, "ShortDelaySeconds": 3, "LongDelaySeconds": 45}
    return [
        Setting("create_profile", create_main, lambda cli: {MAIN: MAIN in profile_names(cli)}, {MAIN: True}),
        Setting("clone_profile", rpc("clone_profile", {"source_name": MAIN, "dest_name": CLONE, "select": False}),
                lambda cli: {CLONE: CLONE in profile_names(cli)}, {CLONE: True}),
        Setting("rename_profile", rpc("rename_profile", {"name": OTHER, "new_name": RENAMED}),
                lambda cli: {RENAMED: RENAMED in profile_names(cli), OTHER: OTHER in profile_names(cli)},
                {RENAMED: True, OTHER: False}),
        Setting("delete_profile", rpc("delete_profile", {"name": DOOMED, "delete_dark_files": True}),
                lambda cli: {DOOMED: DOOMED in profile_names(cli)}, {DOOMED: False},
                prepare=rpc("create_profile", {"name": DOOMED, "select": False})),
        Setting("set_profile", select_main, lambda cli: {"current": cli.call("get_profile")["name"]}, {"current": MAIN},
                prepare=select_other),
        Setting("set_alpaca_server", rpc("set_alpaca_server", alpaca),
                fields("get_alpaca_server", keys=alpaca), alpaca),
        Setting("set_selected_camera", rpc("set_selected_camera", {"camera": cam}),
                fields("get_selected_camera"), {"value": cam}),
        Setting("set_selected_mount", rpc("set_selected_mount", {"mount": mount}),
                fields("get_selected_mount"), {"value": mount}),
        Setting("set_camera_bitdepth", rpc("set_camera_bitdepth", {"bitdepth": 16}),
                fields("get_camera_bitdepth"), {"value": 16}),
        Setting("set_profile_setup", rpc("set_profile_setup", setup), fields("get_profile_setup", keys=setup), setup),
        Setting("set_dark_auto_load", rpc("set_dark_auto_load", {"auto_load_darks": False, "auto_load_defect_map": False}),
                fields("get_calibration_files_status", keys=("auto_load_darks", "auto_load_defect_map")),
                {"auto_load_darks": False, "auto_load_defect_map": False}),
        Setting("set_dither_settings", rpc("set_dither_settings", {"ScaleFactor": 2.5, "RaOnly": True}),
                fields("get_dither_settings", keys=("ScaleFactor", "RaOnly")), {"ScaleFactor": 2.5, "RaOnly": True}),
        Setting("set_star_detection", rpc("set_star_detection", star), fields("get_star_detection", keys=star), star),
        Setting("set_auto_exposure", rpc("set_auto_exposure", {"MinExposure": 700, "MaxExposure": 4500, "TargetSNR": 12.5}),
                fields("get_auto_exposure", keys=("MinExposure", "MaxExposure", "TargetSNR")),
                {"MinExposure": 700, "MaxExposure": 4500, "TargetSNR": 12.5}),
        Setting("set_noise_reduction", rpc("set_noise_reduction", {"method": "2x2 Mean"}),
                fields("get_noise_reduction"), {"value": "2x2 Mean"}),
        Setting("set_guider_options", rpc("set_guider_options", guider), fields("get_guider_options", keys=guider),
                guider),
        Setting("set_variable_delay_settings", rpc("set_variable_delay_settings", delay),
                fields("get_variable_delay_settings", keys=delay), delay),
    ]


def connected_settings():
    """Set and read with the camera and mount connected: they live on the mount, camera and guide algorithms."""

    def algo_params(cli):
        cli.call("set_algo_param", {"axis": "ra", "name": "minMove", "value": 0.29})
        cli.call("set_algo_param", {"axis": "ra", "name": "aggressiveness", "value": 65.0})
        cli.call("set_algo_param", {"axis": "dec", "name": "minMove", "value": 0.37})
        cli.call("set_algo_param", {"axis": "dec", "name": "aggression", "value": 0.85})
        cli.call("set_algo_param", {"axis": "dec", "name": "fastSwitch", "value": 0})

    def read_algo_params(cli):
        got = {}
        for axis, name in (("ra", "minMove"), ("ra", "aggressiveness"), ("dec", "minMove"), ("dec", "aggression"),
                           ("dec", "fastSwitch")):
            got[f"{axis} {name}"] = cli.call("get_algo_param", {"axis": axis, "name": name})
        return got

    mount_opts = {"AssumeOrthogonal": True, "CalFlipRequiresDecFlip": True, "StopGuidingWhenSlewing": False}
    backlash = {"Enabled": True, "PulseWidth": 350, "Floor": 120, "Ceiling": 900}
    limits = {"MaxRaDuration": 1777, "MaxDecDuration": 1888}
    saturation = {"ByADU": True, "SaturationADU": 54321}
    return [
        # set_algo first: set_algo_param below sets the new RA algorithm's parameters
        Setting("set_algo", rpc("set_algo", {"axis": "ra", "name": "Lowpass2"}), fields("get_algo", {"axis": "ra"}),
                {"value": "Lowpass2"}),
        Setting("set_algo_param", algo_params, read_algo_params,
                {"ra minMove": 0.29, "ra aggressiveness": 65.0, "dec minMove": 0.37, "dec aggression": 0.85,
                 "dec fastSwitch": 0}),
        # get_exposure reads 0 while the camera is not connected
        Setting("set_exposure", rpc("set_exposure", {"exposure": 2500}), fields("get_exposure"), {"value": 2500}),
        Setting("set_dec_guide_mode", rpc("set_dec_guide_mode", {"mode": "North"}), fields("get_dec_guide_mode"),
                {"value": "North"}),
        Setting("set_guide_limits", rpc("set_guide_limits", limits), fields("get_guide_limits", keys=limits), limits),
        Setting("set_dec_comp", rpc("set_dec_comp", {"enabled": False}), fields("get_dec_comp"), {"value": False}),
        Setting("set_mount_options", rpc("set_mount_options", mount_opts), fields("get_mount_options", keys=mount_opts),
                mount_opts),
        Setting("set_backlash_comp", rpc("set_backlash_comp", backlash), fields("get_backlash_comp", keys=backlash),
                backlash),
        Setting("set_camera_gain", rpc("set_camera_gain", {"gain": 37}), fields("get_camera_gain", keys=("Gain",)),
                {"Gain": 37}),
        Setting("set_camera_timeout", rpc("set_camera_timeout", {"timeout_ms": 23000}), fields("get_camera_timeout"),
                {"value": 23000}),
        Setting("set_use_subframes", rpc("set_use_subframes", {"enabled": True}), fields("get_use_subframes"),
                {"value": True}),
        Setting("set_camera_saturation", rpc("set_camera_saturation", saturation),
                fields("get_camera_saturation", keys=saturation), saturation),
    ]


def mismatches(setting, cli):
    """The fields of `setting` that do not read as set, as "field got X, set Y" strings."""
    got = setting.read(cli)
    return [f"{k} reads {got.get(k)!r}, set {v!r}" for k, v in setting.want.items() if not same(got.get(k), v)]


def connect(cli):
    cli.call("set_connected", {"connected": True}, timeout=60)
    expect(cli.call("get_connected") is True, "the equipment did not connect")


def daemon_pid(daemon, exe):
    """phd2.bin's pid: the process in the daemon's process group (xvfb-run's) whose executable is `exe`."""
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            if os.getpgid(int(entry)) == daemon.proc.pid and os.path.realpath(f"/proc/{entry}/exe") == exe:
                return int(entry)
        except OSError:
            continue
    raise Failure(f"no {exe} process in the daemon's process group")


def stop_daemon(daemon, sig, exe):
    """Send `sig` to phd2.bin itself, as systemd does on `systemctl stop` (and as a power cut, for SIGKILL, would
    end it); xvfb-run then ends and takes its Xvfb with it. Returns xvfb-run's status, which is phd2.bin's."""
    os.kill(daemon_pid(daemon, exe), sig)
    status = daemon.wait(90)
    daemon.kill()
    # xvfb-run does not wait for its Xvfb, which still holds the display until it is gone; the restart needs it
    deadline = time.time() + 15
    while True:
        try:
            os.killpg(daemon.proc.pid, signal.SIGKILL if time.time() > deadline else 0)
        except ProcessLookupError:
            break
        time.sleep(0.1)
    expect(status is not None, f"the daemon did not exit within 90 s of {sig.name}")
    return status


def check_write_protocol(strace_dir, config_path):
    """The config file written so that a power cut at any point leaves it whole, from strace -ff output (one file
    per thread, so a thread's calls are in order): each rename onto the config is of a file fsynced since the last
    one, the config is never deleted, and the directory is fsynced after each rename, which makes the rename itself
    durable. Returns the number of config writes seen and one line per kind of problem, with a count and an
    example."""
    config_dir = os.path.dirname(config_path)
    fd_path = re.compile(r"^(?:fsync|fdatasync)\(\d+<(.*)>\)\s+=\s+0")
    dirfd = r"(?:AT_FDCWD(?:<[^>]*>)?, )?"  # strace -y shows the directory AT_FDCWD stands for
    rename = re.compile(r'^rename(?:at2?)?\(' + dirfd + r'"([^"]*)", ' + dirfd + r'"([^"]*)"(?:, \w+)?\)\s+=\s+0')
    unlink = re.compile(r'^unlink(?:at)?\(' + dirfd + r'"([^"]*)"(?:, \w+)?\)\s+=\s+0')
    kinds = {
        "deleted": "the config file was deleted before the new one was renamed onto it, so a kill or power cut in "
                   "between leaves no config and the next start makes a new, empty one",
        "no fsync": "the new file was renamed onto the config without an fsync first, so after a power cut the "
                    "config can be empty or hold part of the new data",
        "no dir fsync": f"{config_dir} was not fsynced after the rename, so the rename may not survive a power cut",
        "unparsed": "a rename onto the config this check could not parse",
    }
    found = {}
    writes = 0

    def problem(kind, line):
        found.setdefault(kind, []).append(line.strip())

    for name in sorted(os.listdir(strace_dir)):
        synced = set()  # files fsynced since the last rename onto the config
        pending = None  # a rename onto the config not yet followed by a directory fsync
        with open(os.path.join(strace_dir, name), errors="replace") as f:
            for line in f:
                m = fd_path.match(line)
                if m:
                    synced.add(m.group(1))
                    if m.group(1) == config_dir:
                        pending = None
                    continue
                m = unlink.match(line)
                if m:
                    if m.group(1) == config_path:
                        problem("deleted", line)
                    continue
                m = rename.match(line)
                if m and m.group(2) == config_path:
                    writes += 1
                    if pending:
                        problem("no dir fsync", pending)
                    if m.group(1) not in synced:
                        problem("no fsync", line)
                    synced = set()
                    pending = line
                elif not m and line.startswith("rename") and f'"{config_path}"' in line:
                    problem("unparsed", line)
        if pending:
            problem("no dir fsync", pending)
    return writes, [f"{kinds[k]} ({len(v)} times; e.g. {v[0]})" for k, v in found.items()]


class Pass:
    """One stop signal: set everything, stop with it, start again, read everything back."""

    def __init__(self, args, sig, offset, work):
        self.sig, self.work = sig, work
        self.args = copy.copy(args)
        self.args.instance = args.instance + offset
        self.args.display = str(int(args.display) + offset)
        self.fake_port = args.fake_port + offset
        self.rpc_port = 4400 + self.args.instance - 1
        self.exe = os.path.realpath(args.phd2)
        self.config_path = None
        self.failures = []  # (method or step, message)
        self.passed = []
        self.procs = []  # what to stop on the way out: fakes and daemons
        self.lock = threading.Lock()

    def log(self, msg):
        print(f"[{self.sig.name}] {msg}", flush=True)

    def fail(self, what, msg):
        self.failures.append((what, msg))
        self.log(f"FAIL {what}: {msg}")

    def config_bytes(self):
        try:
            with open(self.config_path, "rb") as f:
                return f.read()
        except FileNotFoundError:
            return None

    def start_daemon(self, n, wrap=()):
        daemon = e2e.Daemon(self.args, self.work, log_name=f"phd2-{n}.out", wrap=wrap)
        with self.lock:
            self.procs.append(daemon)
        cli = e2e.Client(self.rpc_port, daemon, os.path.join(self.work, f"events-{n}.jsonl"), connect_timeout=180)
        return daemon, cli

    def end_daemon(self, daemon, label):
        reports = daemon.sanitizer_reports()
        if reports:
            self.fail(f"sanitizers ({label})", "\n".join(reports[:20]))

    def run(self):
        t0 = time.time()
        os.makedirs(self.work)
        fake = e2e.Fake(self.fake_port, self.work)
        with self.lock:
            self.procs.append(fake)
        before = disconnected_settings(fake)
        after = connected_settings()
        self.config_path = os.path.join(self.work, "home", f".OpenAstroPHD2-instance{self.args.instance}")
        strace_dir = os.path.join(self.work, "strace")
        os.makedirs(strace_dir)
        wrap = ("strace", "-ff", "-qq", "-y", "-s", "4096", "--seccomp-bpf", "-e", f"trace={STRACE_SYSCALLS}", "-o",
                os.path.join(strace_dir, "phd2"))

        # first run: set everything and read it back, then stop with the signal
        daemon, cli = self.start_daemon(1, wrap)
        self.log(f"daemon up on :{self.rpc_port} after {time.time() - t0:.0f} s")
        try:
            applied = []
            for stage, settings in (("disconnected", before), ("connected", after)):
                if stage == "connected":
                    connect(cli)
                for s in settings:
                    try:
                        if s.prepare:
                            s.prepare(cli)
                        on_disk = self.config_bytes()
                        s.apply(cli)
                        # A later call that writes the config would also save this one, and hide it from the
                        # restart below; the SIGKILL pass therefore also checks each call on its own: the file
                        # on disk has to hold the change by the time the call has returned.
                        if self.sig == signal.SIGKILL and self.config_bytes() == on_disk:
                            self.fail(s.method, "the config file on disk was unchanged when the call returned, so a "
                                                "power cut then loses the setting")
                        bad = mismatches(s, cli)
                        if bad:
                            self.fail(s.method, "does not read back as set, before the stop: " + "; ".join(bad))
                        else:
                            applied.append(s)
                    except Failure as e:
                        self.fail(s.method, f"setting it failed: {e}")
            self.log(f"{len(applied)} settings set and read back; stopping phd2.bin with {self.sig.name}")
        finally:
            cli.close()
        status = stop_daemon(daemon, self.sig, self.exe)
        if self.sig == signal.SIGTERM and status != 0:
            self.fail("SIGTERM", f"the daemon exited with status {status} after SIGTERM")
        self.end_daemon(daemon, "first run")

        writes, problems = check_write_protocol(strace_dir, self.config_path)
        if not writes:
            problems.append("strace saw no write of the config file")
        for p in problems:
            self.fail("config write", p)
        self.log(f"strace: {writes} writes of the config file, {len(problems)} kinds of problem")

        # second run on the same HOME and instance: read everything back
        daemon, cli = self.start_daemon(2)
        try:
            lost = 0
            for stage, settings in (("disconnected", before), ("connected", after)):
                if stage == "connected":
                    try:
                        connect(cli)
                    except Failure as e:
                        self.fail("set_connected", f"after the restart: {e}")
                        lost += len(settings)
                        break
                for s in settings:
                    if s not in applied:
                        continue
                    try:
                        bad = mismatches(s, cli)
                    except Failure as e:
                        bad = [f"reading it back failed: {e}"]
                    if bad:
                        lost += 1
                        self.fail(s.method, f"lost after {self.sig.name} and a restart: " + "; ".join(bad))
                    else:
                        self.passed.append(s.method)
            self.log(f"after the restart: {len(self.passed)} settings survived, {lost} lost")
            try:
                cli.call("set_connected", {"connected": False}, timeout=60)
                e2e.shutdown(cli, daemon)
            except Failure as e:
                self.fail("shutdown", str(e))
        finally:
            cli.close()
            daemon.kill()
        self.end_daemon(daemon, "second run")
        e2e.collect_logs(daemon, self.work)
        fake.stop()
        self.log(f"done in {time.time() - t0:.0f} s")

    def stop_all(self):
        with self.lock:
            procs = list(self.procs)
        for p in procs:
            try:
                if isinstance(p, e2e.Daemon):
                    p.kill()
                else:
                    p.stop()
            except Exception:  # best effort on the way out
                pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--phd2", required=True, help="the phd2.bin to run (an ASan/UBSan build in CI)")
    ap.add_argument("--instance", type=int, default=11,
                    help="daemon instance (-i) of the SIGTERM pass; the SIGKILL pass uses the next one up")
    ap.add_argument("--fake-port", type=int, default=11151, help="the SIGTERM pass's fake Alpaca port, and +1")
    ap.add_argument("--display", default="111", help="the SIGTERM pass's X display number, and +1")
    ap.add_argument("--work-dir", required=True, help="the scratch HOMEs and every log go here (emptied first)")
    ap.add_argument("--timeout", type=float, default=900, help="the whole run, seconds")
    args = ap.parse_args()
    expect(shutil.which("strace") is not None, "strace is needed to check how the config file is written")

    work = os.path.abspath(args.work_dir)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    t0 = time.time()
    passes = [Pass(args, signal.SIGTERM, 0, os.path.join(work, "sigterm")),
              Pass(args, signal.SIGKILL, 1, os.path.join(work, "sigkill"))]

    def run(p):
        try:
            p.run()
        except Failure as e:
            p.fail("run", str(e))
        except Exception as e:  # a bug in a check fails the pass, and the processes are still stopped
            p.fail("run", repr(e))
            traceback.print_exc()
        finally:
            p.stop_all()

    threads = [threading.Thread(target=run, args=(p,), daemon=True) for p in passes]
    for t in threads:
        t.start()
    deadline = t0 + args.timeout
    for t in threads:
        t.join(max(0.0, deadline - time.time()))
    if any(t.is_alive() for t in threads):
        for p in passes:
            p.fail("run", f"the run took longer than {args.timeout:.0f} s")
            p.stop_all()
        for t in threads:
            t.join(30)

    print("")
    failed = sum(len(p.failures) for p in passes)
    print(f"persistence: {sum(len(p.passed) for p in passes)} settings survived, {failed} failures, "
          f"{time.time() - t0:.0f} s; logs in {work}")
    for p in passes:
        for what, msg in p.failures:
            print(f"- {p.sig.name} {what}: {msg}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
