#!/usr/bin/env python3
import argparse
import json
import math
import socket
import sys
import time
import urllib.parse
import urllib.request


class RpcClient:
    def __init__(self, host: str, port: int, timeout: float):
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.settimeout(timeout)
        self._file = self._sock.makefile("rwb", buffering=0)
        self._id = 1
        self.last_events = []  # the events that arrived while request() waited for its response

    def close(self):
        try:
            self._file.close()
        finally:
            self._sock.close()

    def _next_id(self):
        i = self._id
        self._id += 1
        return i

    def call(self, method: str, params=None, timeout_s: float = 5.0):
        msg = self.request(method, params=params, timeout_s=timeout_s)
        if "error" in msg and msg["error"]:
            raise RuntimeError(f"{method} error: {msg['error']}")
        return msg.get("result")

    def request(self, method: str, params=None, timeout_s: float = 5.0):
        req_id = self._next_id()
        req = {"jsonrpc": "2.0", "method": method, "id": req_id}
        if params is not None:
            req["params"] = params
        self._file.write((json.dumps(req) + "\n").encode("utf-8"))

        deadline = time.time() + timeout_s
        self.last_events = []
        while time.time() < deadline:
            line = self._file.readline()
            if not line:
                raise RuntimeError(f"socket closed while waiting for {method}")
            msg = json.loads(line.decode("utf-8", errors="replace").strip())
            if isinstance(msg, dict) and msg.get("id") == req_id:
                return msg
            if isinstance(msg, dict) and "Event" in msg:
                self.last_events.append(msg["Event"])
        raise RuntimeError(f"timeout waiting for response to {method}")

    def raw_request(self, line: str, match, timeout_s: float = 5.0):
        """Send one raw request line and return the first message match(msg) accepts."""
        self._file.write((line + "\n").encode("utf-8"))

        deadline = time.time() + timeout_s
        while time.time() < deadline:
            resp = self._file.readline()
            if not resp:
                raise RuntimeError(f"socket closed while waiting for a reply to {line}")
            msg = json.loads(resp.decode("utf-8", errors="replace").strip())
            if match(msg):
                return msg
        raise RuntimeError(f"timeout waiting for a reply to {line}")


class FakeAlpaca:
    """The control endpoints of scripts/fake_alpaca_camera.py."""

    def __init__(self, url: str):
        self.url = url.rstrip("/")
        parsed = urllib.parse.urlparse(self.url)
        self.host, self.port = parsed.hostname, parsed.port or 80

    def get(self, path: str):
        with urllib.request.urlopen(self.url + path, timeout=5) as r:
            return json.loads(r.read().decode())

    def pulses(self, clear=False):
        return self.get("/fake/pulses" + ("?clear=1" if clear else ""))["pulses"]

    def declination(self):
        """The fake mount's Dec, degrees, as the guider reads it."""
        return self.get("/api/v1/telescope/0/declination")["Value"]


def alpaca_port_configured(cfg):
    """True when a get_alpaca_server result names a usable port (a profile with no server set reads port 0)."""
    port = cfg.get("port") if isinstance(cfg, dict) else None
    return isinstance(port, int) and 1 <= port <= 65535


def expect_equal(got, want):
    if got != want:
        raise RuntimeError(f"got {got!r}, expected {want!r}")
    return got


def expect_typed(got, want):
    """got == want, and each value of the dict has the JSON type of want's: 3 is not 3.0, True is not 1."""
    expect_equal(got, want)
    for k, v in want.items():
        if type(got[k]) is not type(v):
            raise RuntimeError(f"{k} is {got[k]!r} ({type(got[k]).__name__}), expected a {type(v).__name__}")
    return got


def expect_list(value, key):
    if not isinstance(value, list):
        raise RuntimeError(f"{key}: expected a list, got {value!r}")
    return f"{len(value)} choices"


def expect_absent(obj, key):
    if key in obj:
        raise RuntimeError(f"unexpected key {key}: {obj[key]!r}")
    return "absent"


def alpaca_query_live_check(cli, fake, run_check):
    """#155: query_alpaca_devices lists the camera and the telescope that scripts/fake_alpaca_camera.py serves."""

    def query():
        devices = cli.call("query_alpaca_devices", {"host": fake.host, "port": fake.port, "device_type": "all"})
        found = sorted((d.get("device_type"), d.get("device_number")) for d in devices or [])
        expect_equal(found, [("Camera", 0), ("Telescope", 0)])
        cameras = cli.call("query_alpaca_devices", {"host": fake.host, "port": fake.port, "device_type": "camera"})
        expect_equal([d.get("device_type") for d in cameras or []], ["Camera"])
        return devices

    run_check("query_alpaca_devices against the fake server", query)


def ra_calibration_step_ms(step_ms, dec_deg):
    """#141: the West/East calibration pulse for a step at a Dec (calibration_run.cpp RaCalibrationStepMs)."""
    c = max(abs(math.cos(math.radians(dec_deg))), 1.0 / 4.0)
    return max(step_ms, math.ceil(step_ms / c))


def setter_latency_check(cli, calls=50, limit_ms=10.0):
    """A setter is answered about as fast as a getter (#175). A setter sends a ConfigurationChange event and then its
    response, two small writes on the client's socket; without TCP_NODELAY, Nagle held the response until the client
    acknowledged the event, and Linux delays that ACK by about 40 ms, so every setter took about 45 ms against about
    0.1 ms for a getter. The setter here writes back the profile's own RaOnly, so it changes nothing and writes no
    config file: what is timed is the round trip, not a disk write. ConfigurationChange is sent at most once per pass
    of the main loop, so calls answered at once can see fewer events than calls; a call that waited for the ACK
    always lets the next one send its event, so without TCP_NODELAY every call has one and is delayed."""
    ra_only = cli.call("get_dither_settings")["RaOnly"]

    def median_ms(method, params):
        times = []
        events = 0
        for _ in range(calls):
            t0 = time.perf_counter()
            msg = cli.request(method, params)
            times.append((time.perf_counter() - t0) * 1000.0)
            if msg.get("error"):
                raise RuntimeError(f"{method} error: {msg['error']}")
            events += "ConfigurationChange" in cli.last_events
        times.sort()
        return times[len(times) // 2], times[-1], events

    get_med, get_max, _ = median_ms("get_dither_settings", None)
    set_med, set_max, set_events = median_ms("set_dither_settings", {"RaOnly": ra_only})
    summary = (f"set_dither_settings median {set_med:.2f} ms (max {set_max:.2f}), get_dither_settings median "
               f"{get_med:.2f} ms (max {get_max:.2f}), {calls} calls each, {set_events} ConfigurationChange events")
    if set_med > limit_ms:
        raise RuntimeError(f"setter median over {limit_ms:g} ms: {summary}")
    return summary


def profile_setup_live_checks(cli, fake, run_check):
    """#117: set_profile_setup values reach the live mount and camera objects, with no restart.

    Runs in temporary profiles (no saved calibration, binning 1, so BeginCalibration keeps the step it is given)
    against scripts/fake_alpaca_camera.py, and needs the equipment disconnected. Restores the current profile."""
    base = cli.call("get_profile")
    stamp = int(time.time())
    temp_name, other_name = f"rpc_smoke_setup_{stamp}", f"rpc_smoke_setup_{stamp}_other"
    cam_choice = f"Alpaca Camera [{fake.host}:{fake.port}/0]"
    mount_choice = f"Alpaca Mount [{fake.host}:{fake.port}/0]"

    def expect(cond, msg):
        if not cond:
            raise RuntimeError(msg)

    def connect():
        cli.call("set_connected", {"connected": True}, timeout_s=30)
        expect(cli.call("get_connected") is True, "equipment did not connect")

    def disconnect():
        cli.call("set_connected", {"connected": False}, timeout_s=30)

    def wait_stopped():
        """stop_capture returns before the capture loop has ended."""
        deadline = time.time() + 30
        while cli.call("get_app_state") != "Stopped":
            if time.time() > deadline:
                raise RuntimeError(f"capture still {cli.call('get_app_state')} 30 s after stop_capture")
            time.sleep(0.25)

    def calibration_pulses(want):
        """Start a forced calibration and return the first `want` pulses the mount received."""
        fake.pulses(clear=True)
        cli.call("guide", {"settle": {"pixels": 1.5, "time": 1, "timeout": 60}, "recalibrate": True}, timeout_s=30)
        try:
            deadline = time.time() + 90
            while time.time() < deadline:
                got = fake.pulses()
                if len(got) >= want:
                    return got[:want]
                time.sleep(0.5)
            raise RuntimeError(f"no calibration pulses within 90 s (got {fake.pulses()})")
        finally:
            cli.call("stop_capture")
            wait_stopped()

    def step_check(duration, distance):
        cli.call("set_profile_setup", {"calibration_duration": duration, "calibration_distance": distance})
        try:
            connect()
            got = calibration_pulses(2)
        finally:
            disconnect()
        # Alpaca GuideDirections: 3 = West, the first calibration leg, which pulses the step sized for the Dec (#141)
        want = ra_calibration_step_ms(duration, fake.declination())
        expect(all(p["direction"] == 3 and p["duration"] == want for p in got),
               f"expected West pulses of {want} ms (step {duration} ms), got {got}")
        return got

    def scale_check(pixel_size, focal_length):
        # while disconnected: at connect the Alpaca camera's own pixel size replaces the profile's
        cli.call("set_profile_setup", {"pixel_size": pixel_size, "focal_length": focal_length})
        scale = cli.call("get_pixel_scale")
        want = 206.265 * pixel_size / focal_length
        expect(isinstance(scale, (int, float)) and abs(scale - want) < 0.01,
               f"pixel scale {scale}, expected {want:.3f} for {pixel_size} um at {focal_length} mm")
        return scale

    def binning_check(software_binning):
        cli.call("set_profile_setup", {"software_binning": software_binning})
        try:
            connect()
            binning = cli.call("get_camera_binning")
        finally:
            disconnect()
        expect(binning == software_binning, f"camera binning {binning}, expected {software_binning}")
        return binning

    try:
        cli.call("create_profile", {"name": temp_name, "select": True})
        cli.call("set_alpaca_server", {"host": fake.host, "port": fake.port, "camera_device": 0, "telescope_device": 0})
        cli.call("set_selected_camera", {"camera": cam_choice})
        cli.call("set_selected_mount", {"mount": mount_choice})
        cli.call("set_exposure", {"exposure": 500})

        run_check("#117 calibration step 750 ms", lambda: step_check(750, 25))
        run_check("#117 calibration step 1500 ms without restart", lambda: step_check(1500, 30))

        run_check("#117 pixel size 2.4 um", lambda: scale_check(2.4, 400))
        run_check("#117 pixel size 5.0 um without restart", lambda: scale_check(5.0, 400))

        run_check("#117 software binning 2 without restart", lambda: binning_check(2))
        run_check("#117 software binning 1 without restart", lambda: binning_check(1))

        # delete_profile of the current profile rebuilds the gear of the profile that is current after it, as a
        # profile switch does (set_profile to that id returns at once, so it could not)
        def delete_current_check():
            cli.call("delete_profile", {"name": temp_name, "delete_dark_files": True})
            after_delete = cli.call("get_current_equipment")
            current = cli.call("get_profile")
            cli.call("create_profile", {"name": other_name, "select": True})
            cli.call("set_profile", {"id": current["id"]})
            after_switch = cli.call("get_current_equipment")
            cli.call("delete_profile", {"name": other_name, "delete_dark_files": True})
            expect(after_delete == after_switch,
                   f"gear after deleting the current profile {after_delete} != after a switch {after_switch}")
            return after_delete

        run_check("#117 delete_profile of the current profile rebuilds the gear", delete_current_check)
    finally:
        try:
            cli.call("stop_capture")
            wait_stopped()
            cli.call("set_connected", {"connected": False}, timeout_s=30)
            cli.call("set_profile", {"id": base["id"]})
            names = [p.get("name") for p in cli.call("get_profiles") or []]
            for name in (temp_name, other_name):
                if name in names:
                    cli.call("delete_profile", {"name": name, "delete_dark_files": True})
        except Exception as e:
            print(f"WARN #117 cleanup: {e}")


def clear_calibration_live_checks(cli, fake, run_check, calibration_timeout=180.0):
    """#137: clear_calibration is refused while calibrating and the calibration goes on; allowed while guiding.

    On main the clear went through, and the calibration it cleared never finished (an assert abort with asserts
    compiled in, Calibrating forever without). Runs in a temporary profile against scripts/fake_alpaca_camera.py and
    needs the equipment disconnected. Restores the current profile."""
    base = cli.call("get_profile")
    temp_name = f"rpc_smoke_clearcal_{int(time.time())}"

    def expect(cond, msg):
        if not cond:
            raise RuntimeError(msg)

    def wait_state(want, timeout_s):
        deadline = time.time() + timeout_s
        while True:
            state = cli.call("get_app_state")
            if state == want:
                return
            if time.time() > deadline:
                raise RuntimeError(f"app state {state}, expected {want} within {timeout_s} s")
            time.sleep(0.25)

    def refused_while_calibrating():
        cli.call("guide", {"settle": {"pixels": 1.5, "time": 1, "timeout": 120}, "recalibrate": True}, timeout_s=30)
        wait_state("Calibrating", 60)
        err = cli.request("clear_calibration").get("error")
        expect(err and err.get("code") == 1, f"clear_calibration while calibrating: expected error 1, got {err}")
        expect(cli.call("get_app_state") == "Calibrating", "the refused clear_calibration ended the calibration")
        wait_state("Guiding", calibration_timeout)
        expect(cli.call("get_calibrated") is True, "not calibrated after the calibration completed")
        return err

    def allowed_while_guiding():
        expect(cli.call("get_app_state") == "Guiding", "not guiding")
        expect(cli.call("clear_calibration") == 0, "clear_calibration while guiding did not return 0")
        expect(cli.call("get_app_state") == "Guiding", "guiding stopped after clear_calibration")
        expect(cli.call("get_calibrated") is False, "still calibrated after clear_calibration")
        # guide corrections go on with the old calibration; dither needs one
        err = cli.request("dither", {"amount": 3, "settle": {"pixels": 1.5, "time": 1, "timeout": 30}}).get("error")
        expect(err and err.get("code") == 1, f"dither after clear_calibration: expected error 1, got {err}")
        return "cleared; still guiding; dither refused"

    try:
        cli.call("create_profile", {"name": temp_name, "select": True})
        cli.call("set_alpaca_server", {"host": fake.host, "port": fake.port, "camera_device": 0, "telescope_device": 0})
        cli.call("set_selected_camera", {"camera": f"Alpaca Camera [{fake.host}:{fake.port}/0]"})
        cli.call("set_selected_mount", {"mount": f"Alpaca Mount [{fake.host}:{fake.port}/0]"})
        cli.call("set_exposure", {"exposure": 500})
        cli.call("set_connected", {"connected": True}, timeout_s=30)

        if run_check("#137 clear_calibration refused while calibrating", refused_while_calibrating) is not None:
            run_check("#137 clear_calibration allowed while guiding", allowed_while_guiding)
    finally:
        try:
            cli.call("stop_capture")
            wait_state("Stopped", 30)
            cli.call("set_connected", {"connected": False}, timeout_s=30)
            cli.call("set_profile", {"id": base["id"]})
            if temp_name in [p.get("name") for p in cli.call("get_profiles") or []]:
                cli.call("delete_profile", {"name": temp_name, "delete_dark_files": True})
        except Exception as e:
            print(f"WARN #137 cleanup: {e}")


def main():
    ap = argparse.ArgumentParser(description="PHD2 JSON-RPC smoke test for PINS/NINA compatibility methods")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4400)
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--calibration-timeout", type=float, default=180.0,
                    help="seconds a calibration in the --fake-alpaca-url checks may take (raise it for a sanitizer build)")
    ap.add_argument("--fake-alpaca-url", metavar="URL",
                    help="also check query_alpaca_devices (#155), that set_profile_setup reaches the live mount and "
                         "camera (#117), and clear_calibration during a calibration (#137), against "
                         "scripts/fake_alpaca_camera.py at URL "
                         "(e.g. http://127.0.0.1:11111); needs the equipment disconnected")
    args = ap.parse_args()

    failures = []
    checks = 0

    def run_check(name, fn):
        nonlocal checks
        checks += 1
        try:
            result = fn()
            print(f"PASS {name}: {result}")
            return result
        except Exception as e:
            failures.append((name, str(e)))
            print(f"FAIL {name}: {e}")
            return None

    def expect_error(name, method, params=None, code=None, message_contains=None):
        nonlocal checks
        checks += 1
        try:
            msg = cli.request(method, params=params)
            err = msg.get("error")
            if not err:
                failures.append((name, f"expected error, got success: {msg.get('result')}"))
                print(f"FAIL {name}: expected error, got success")
                return
            if code is not None and err.get("code") != code:
                failures.append((name, f"expected code {code}, got {err.get('code')}"))
                print(f"FAIL {name}: expected code {code}, got {err.get('code')}")
                return
            if message_contains and message_contains.lower() not in str(err.get("message", "")).lower():
                failures.append((name, f"error message missing '{message_contains}': {err}"))
                print(f"FAIL {name}: error message missing '{message_contains}'")
                return
            print(f"PASS {name}: {err}")
        except Exception as e:
            failures.append((name, str(e)))
            print(f"FAIL {name}: {e}")

    def expect_raw_error(name, line, req_id, code):
        # #118: a request refused before dispatch must be answered with its own id
        nonlocal checks
        checks += 1
        try:
            if req_id is None:
                match = lambda m: isinstance(m, dict) and "Event" not in m and "error" in m and m.get("id") is None
            else:
                match = lambda m: isinstance(m, dict) and "Event" not in m and m.get("id") == req_id
            msg = cli.raw_request(line, match)
            err = msg.get("error")
            if not err:
                failures.append((name, f"expected error, got success: {msg.get('result')}"))
                print(f"FAIL {name}: expected error, got success")
                return
            if err.get("code") != code:
                failures.append((name, f"expected code {code}, got {err.get('code')}"))
                print(f"FAIL {name}: expected code {code}, got {err.get('code')}")
                return
            print(f"PASS {name}: {err}")
        except Exception as e:
            failures.append((name, str(e)))
            print(f"FAIL {name}: {e}")

    try:
        cli = RpcClient(args.host, args.port, args.timeout)
    except Exception as e:
        print(f"FAIL connect {args.host}:{args.port}: {e}")
        return 2

    try:
        run_check("get_app_state", lambda: cli.call("get_app_state"))
        run_check("get_profiles", lambda: cli.call("get_profiles"))
        run_check("get_profile", lambda: cli.call("get_profile"))
        run_check("get_profile_setup", lambda: cli.call("get_profile_setup"))
        run_check("get_connected", lambda: cli.call("get_connected"))
        run_check("get_current_equipment", lambda: cli.call("get_current_equipment"))
        run_check("get_calibration_files_status", lambda: cli.call("get_calibration_files_status"))

        # the daemon is Alpaca-only (#115): the INDI methods are gone from the method table (#155)
        for method, params in (
            ("get_indi_server", None),
            ("set_indi_server", {"host": "localhost", "port": 7624}),
            ("get_selected_indi_mount_driver", None),
            ("set_selected_indi_mount_driver", {"mount_driver": "indi_simulator_telescope"}),
            ("get_selected_indi_camera_driver", None),
            ("set_selected_indi_camera_driver", {"camera_driver": "indi_simulator_ccd"}),
        ):
            expect_error(f"{method}_removed_returns_method_not_found", method, params=params, code=-32601)

        alpaca_cfg = run_check("get_alpaca_server", lambda: cli.call("get_alpaca_server"))
        run_check("discover_alpaca_servers", lambda: cli.call("discover_alpaca_servers", {"num_queries": 1, "timeout_seconds": 1}))
        if alpaca_port_configured(alpaca_cfg):
            run_check("query_alpaca_devices", lambda: cli.call("query_alpaca_devices", {"device_type": "all"}))
        else:
            # a profile with no Alpaca server set has port 0, so the query goes to <host>:0 and fails (#155)
            expect_error(
                "query_alpaca_devices_unconfigured_profile_fails",
                "query_alpaca_devices",
                params={"device_type": "all"},
                code=1,
                message_contains="failed to query configured devices",
            )
        expect_error(
            "query_alpaca_devices_port_0_returns_invalid_params",
            "query_alpaca_devices",
            params={"host": "127.0.0.1", "port": 0},
            code=-32602,
        )
        expect_error(
            "query_alpaca_devices_unknown_type_returns_invalid_params",
            "query_alpaca_devices",
            params={"device_type": "focuser"},
            code=-32602,
        )

        choices = run_check("get_equipment_choices", lambda: cli.call("get_equipment_choices")) or {}
        mount_choice = run_check("get_selected_mount", lambda: cli.call("get_selected_mount"))
        camera_choice = run_check("get_selected_camera", lambda: cli.call("get_selected_camera"))
        rot_choice = run_check("get_selected_rotator", lambda: cli.call("get_selected_rotator"))

        # no aux mount and no AO in this build: the getters read "None" and the setters are refused (#155)
        run_check("get_selected_aux_mount is None", lambda: expect_equal(cli.call("get_selected_aux_mount"), "None"))
        run_check("get_selected_ao is None", lambda: expect_equal(cli.call("get_selected_ao"), "None"))
        expect_error(
            "set_selected_aux_mount_not_supported",
            "set_selected_aux_mount",
            params={"aux_mount": "None"},
            code=1,
            message_contains="not supported",
        )
        expect_error(
            "set_selected_ao_not_supported",
            "set_selected_ao",
            params={"ao": "None"},
            code=1,
            message_contains="not supported",
        )

        run_check("get_selected_camera_id", lambda: cli.call("get_selected_camera_id"))

        bitdepth = run_check("get_camera_bitdepth", lambda: cli.call("get_camera_bitdepth"))

        if isinstance(mount_choice, str):
            run_check("set_selected_mount", lambda: cli.call("set_selected_mount", {"mount": mount_choice}))
        if isinstance(camera_choice, str):
            run_check("set_selected_camera", lambda: cli.call("set_selected_camera", {"camera": camera_choice}))
        if isinstance(rot_choice, str):
            run_check("set_selected_rotator", lambda: cli.call("set_selected_rotator", {"rotator": rot_choice}))

        if isinstance(bitdepth, int):
            run_check("set_camera_bitdepth", lambda: cli.call("set_camera_bitdepth", {"bitdepth": bitdepth}))

        current_cam_id = run_check("get_selected_camera_id (again)", lambda: cli.call("get_selected_camera_id"))
        if isinstance(current_cam_id, str):
            run_check("set_selected_camera_id", lambda: cli.call("set_selected_camera_id", {"camera_id": current_cam_id}))

        still_connected = run_check("get_connected (config checks)", lambda: cli.call("get_connected"))
        if not still_connected:
            alpaca_cfg = run_check("get_alpaca_server (again)", lambda: cli.call("get_alpaca_server"))
            if isinstance(alpaca_cfg, dict):
                # write the profile's own values back; the port only when it is set, because the port 0 of a profile
                # with no Alpaca server is refused (#155, checked below)
                keys = ["host", "camera_device", "telescope_device", "rotator_device"]
                if alpaca_port_configured(alpaca_cfg):
                    keys.append("port")
                payload = {k: alpaca_cfg[k] for k in keys if k in alpaca_cfg}
                if payload:
                    run_check("set_alpaca_server", lambda: cli.call("set_alpaca_server", payload))
                    run_check(
                        "get_alpaca_server unchanged by set_alpaca_server",
                        lambda: expect_equal(cli.call("get_alpaca_server"), alpaca_cfg),
                    )
            expect_error(
                "set_alpaca_server_port_0_returns_invalid_params",
                "set_alpaca_server",
                params={"port": 0},
                code=-32602,
                message_contains="port in range",
            )

        # the equipment lists: camera, mount and rotator; no aux mount or AO lists in this build (#155)
        for key in ("camera", "mount", "rotator"):
            run_check(f"get_equipment_choices has {key}", lambda key=key: expect_list(choices.get(key), key))
        for key in ("aux_mount", "AO"):
            run_check(f"get_equipment_choices has no {key}", lambda key=key: expect_absent(choices, key))

        expect_error(
            "unknown_method_returns_method_not_found",
            "this_method_does_not_exist_123",
            code=-32601,
        )
        expect_error(
            "set_selected_mount_invalid_type",
            "set_selected_mount",
            params={"mount": 123},
            code=-32602,
        )
        expect_error(
            "set_selected_camera_invalid_choice",
            "set_selected_camera",
            params={"camera": "__NOT_A_REAL_CAMERA__"},
            code=-32602,
        )

        # params that are not an array or object are refused before the method
        # runs, so none of these touches the equipment (#118)
        for method in ("set_exposure", "guide", "set_connected", "get_app_state"):
            for label, params in (("number", "2000"), ("string", '"2000"'), ("null", "null")):
                req_id = cli._next_id()
                expect_raw_error(
                    f"{method}_{label}_params_returns_invalid_params_with_id",
                    f'{{"jsonrpc":"2.0","method":"{method}","params":{params},"id":{req_id}}}',
                    req_id,
                    -32602,
                )
        expect_raw_error(
            "string_id_is_echoed_on_invalid_params",
            '{"jsonrpc":"2.0","method":"set_exposure","params":2000,"id":"smoke-118"}',
            "smoke-118",
            -32602,
        )
        req_id = cli._next_id()
        expect_raw_error(
            "missing_method_returns_invalid_request_with_id",
            f'{{"jsonrpc":"2.0","params":[],"id":{req_id}}}',
            req_id,
            -32600,
        )
        expect_raw_error("empty_batch_returns_invalid_request", "[]", None, -32600)

        # set_algo_param without a value crashed the guider (#138). The value is
        # checked before the algorithm is looked up, so these change nothing.
        expect_error(
            "set_algo_param_without_value_returns_invalid_params",
            "set_algo_param",
            params={"axis": "ra", "name": "minMove"},
            code=-32602,
        )
        expect_error(
            "set_algo_param_string_value_returns_invalid_params",
            "set_algo_param",
            params={"axis": "ra", "name": "minMove", "value": "0.2"},
            code=-32602,
        )
        run_check("get_app_state (after set_algo_param without value)", lambda: cli.call("get_app_state"))

        is_connected = run_check("get_connected (final)", lambda: cli.call("get_connected"))
        if is_connected and isinstance(camera_choice, str):
            expect_error(
                "set_selected_camera_rejected_while_connected",
                "set_selected_camera",
                params={"camera": camera_choice},
                message_contains="connected",
            )
        elif not is_connected:
            base_profile = run_check("get_profile (before profile CRUD)", lambda: cli.call("get_profile")) or {}
            base_id = base_profile.get("id")
            test_name = f"rpc_smoke_profile_{int(time.time())}"
            test_name2 = test_name + "_renamed"
            test_name3 = test_name + "_clone"

            created = run_check("create_profile", lambda: cli.call("create_profile", {"name": test_name, "select": True})) or {}
            run_check("set_profile_by_name", lambda: cli.call("set_profile_by_name", {"name": test_name}))
            run_check("rename_profile", lambda: cli.call("rename_profile", {"name": test_name, "new_name": test_name2}))
            run_check("clone_profile", lambda: cli.call("clone_profile", {"source_name": test_name2, "dest_name": test_name3, "select": False}))

            run_check(
                "set_profile_setup",
                lambda: cli.call(
                    "set_profile_setup",
                    {
                        "focal_length": 400,
                        "pixel_size": 2.4,
                        "camera_binning": 1,
                        "software_binning": 1,
                        "guide_speed": 0.5,
                        "calibration_duration": 750,
                        "calibration_distance": 25,
                        "high_res_encoders": False,
                        "auto_restore_calibration": False,
                        "multistar_enabled": True,
                    },
                ),
            )

            # the delays are kept to the ms (#174): 2.5 s used to be stored as 2000 ms and read back as 2. Whole
            # seconds still read back as JSON integers, the type PHD2 documents.
            for label, delay in (("fractional", {"Enabled": True, "ShortDelaySeconds": 2.5, "LongDelaySeconds": 7.25}),
                                 ("whole", {"Enabled": False, "ShortDelaySeconds": 3, "LongDelaySeconds": 45})):
                run_check(f"set_variable_delay_settings {label} seconds",
                          lambda delay=delay: cli.call("set_variable_delay_settings", delay))
                run_check(f"get_variable_delay_settings reads back {label} seconds",
                          lambda delay=delay: expect_typed(cli.call("get_variable_delay_settings"), delay))

            # get_exposure reads the selected exposure with the camera disconnected too (#176); it read 0
            for exposure in (2500, 1000):
                run_check(f"set_exposure {exposure} while disconnected",
                          lambda exposure=exposure: cli.call("set_exposure", {"exposure": exposure}))
                run_check(f"get_exposure reads {exposure} while disconnected",
                          lambda exposure=exposure: expect_equal(cli.call("get_exposure"), exposure))

            run_check("delete_profile clone", lambda: cli.call("delete_profile", {"name": test_name3, "delete_dark_files": False}))
            run_check("delete_profile renamed", lambda: cli.call("delete_profile", {"name": test_name2, "delete_dark_files": False}))
            if isinstance(base_id, int) and base_id > 0:
                run_check("restore_profile", lambda: cli.call("set_profile", {"id": base_id}))

        run_check("setter latency", lambda: setter_latency_check(cli))

        if args.fake_alpaca_url:
            if cli.call("get_connected"):
                failures.append(("#117 profile setup checks", "equipment is connected; disconnect it first"))
                print("FAIL #117 profile setup checks: equipment is connected; disconnect it first")
            else:
                alpaca_query_live_check(cli, FakeAlpaca(args.fake_alpaca_url), run_check)
                profile_setup_live_checks(cli, FakeAlpaca(args.fake_alpaca_url), run_check)
                clear_calibration_live_checks(cli, FakeAlpaca(args.fake_alpaca_url), run_check, args.calibration_timeout)

    finally:
        cli.close()

    if failures:
        print("")
        print(f"SMOKE RESULT: FAIL ({len(failures)}/{checks} checks failed)")
        for name, err in failures:
            print(f"- {name}: {err}")
        return 1

    print("")
    print(f"SMOKE RESULT: PASS ({checks} checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
