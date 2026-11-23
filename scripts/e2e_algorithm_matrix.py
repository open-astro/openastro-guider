#!/usr/bin/env python3
"""The daemon-level slice of the guide algorithm matrix (#142 Tier 4).

The wide matrix runs in process (tests/benchmark/algorithm_matrix.cpp, the real wx-free kernels in a
seeded closed loop). This is the narrow slice through the daemon itself: phd2.bin against
scripts/fake_alpaca_camera.py with its mount and sky physics, driven over JSON-RPC as Ara drives it. It
covers what the in-process matrix cannot: ZFilter and Predictive PEC (whose math is not extracted from
the daemon), the algorithm swap while guiding (set_algo, #142 T2b), and the whole frame path (star
finding on rendered frames, the Alpaca pulses).

Each lane is a fake with one set of physics and its own daemon (instance and fake port + the lane's
index), run at the same time as the others. A lane connects, slews to its Dec, calibrates once (guide
with recalibrate true) and then, guiding all the while, goes through the cells: for each RA/Dec
algorithm pair it calls set_algo on the axes that change, lets the new algorithm settle in for
--warmup-steps GuideSteps, then measures --steps GuideSteps. The same target throughout, so one
calibration serves all the cells; a new target would recalibrate.

What a cell reports, from the fake's GET /fake/frames (the truth at each frame drawn, without the
seeing): the standard deviation of the mount's RA and Dec offsets (px, mount axes) over the measured
frames, what an imaging camera on the mount would have seen; and the same from the GuideStep events'
RADistanceRaw / DECDistanceRaw, what the guider measured (seeing included). The fake's simulated time
advances by each exposure and pulse, so the sky does not depend on how fast the host runs.

With --thresholds it fails a cell whose true total RMS is over its ceiling in the CSV (lane, RA
algorithm, Dec algorithm, max_true_total_px), and a cell that lost guiding. The ceilings are generous
(--write-thresholds writes 1.5 x the measured + 0.1 px): the slice catches an algorithm that stopped
working through the daemon, not a few percent.

    python3 scripts/e2e_algorithm_matrix.py --phd2 tmp/phd2.bin --work-dir /tmp/matrix \\
        --thresholds tests/benchmark/daemon_matrix_thresholds.csv --markdown /tmp/matrix/summary.md
    python3 scripts/e2e_algorithm_matrix.py ... --lanes nominal --cells "Hysteresis/Resist Switch,ZFilter/Resist Switch"
"""
import argparse
import csv
import json
import math
import os
import shutil
import sys
import threading
import time
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import e2e_guide_scenarios as e2e  # noqa: E402  (the daemon, fake and JSON-RPC client classes)

# The fake's pulses move the field at 1x sidereal converted with this scale (fake_alpaca_camera.py's
# --image-scale default), so it is the arcsec per px of the fake's sky
FAKE_ARCSEC_PER_PX = 7.73

# The lanes: one fake each. Simulated drift and PE are in px of the fake's sensor (7.73"/px).
LANES = {
    # a good night: light seeing, a 1.5 px (11.6") RA periodic error over 240 s, a polar alignment drift
    "nominal": {
        "dec": 30.0,
        "fake_args": ("--imagebytes", "--camera-angle", "37", "--cos-dec", "--sim-drift", "0.2,0.4",
                      "--pe-px", "1.5", "--pe-period-s", "240", "--seeing-px", "0.1", "--frame-noise",
                      "--seed", "142"),
    },
    # a rough one: three times the seeing and 800 ms of Dec backlash, at Dec 60
    "rough": {
        "dec": 60.0,
        "fake_args": ("--imagebytes", "--camera-angle", "37", "--cos-dec", "--dec-backlash-ms", "800",
                      "--sim-drift", "0.2,0.4", "--pe-px", "1.5", "--pe-period-s", "240", "--seeing-px", "0.3",
                      "--frame-noise", "--seed", "143"),
    },
}

# (RA algorithm, Dec algorithm), as set_algo names them: each RA algorithm with the Dec default, then each
# Dec algorithm with the RA default. Predictive PEC is RA only.
CELLS = (
    ("Hysteresis", "Resist Switch"),
    ("Lowpass", "Resist Switch"),
    ("Lowpass2", "Resist Switch"),
    ("Resist Switch", "Resist Switch"),
    ("ZFilter", "Resist Switch"),
    ("Predictive PEC", "Resist Switch"),
    ("Hysteresis", "Hysteresis"),
    ("Hysteresis", "Lowpass"),
    ("Hysteresis", "Lowpass2"),
    ("Hysteresis", "ZFilter"),
)

EXPOSURE_MS = 1000  # RC51 guides at 1 s; the daemon exposes in real time, so this sets the wall time too


def sigma(values):
    if not values:
        return float("nan")
    mean = sum(values) / len(values)
    return math.sqrt(max(0.0, sum((v - mean) ** 2 for v in values) / len(values)))


def run_cell(cli, fake, ra_algo, dec_algo, args):
    """Switch to the pair while guiding, warm up, measure. Returns the cell's result dict."""
    t0 = time.time()
    for axis, name in (("ra", ra_algo), ("dec", dec_algo)):
        if cli.call("get_algo", {"axis": axis}) != name:
            cli.call("set_algo", {"axis": axis, "name": name})
        got = cli.call("get_algo", {"axis": axis})
        e2e.expect(got == name, f"get_algo {axis} is {got!r} after set_algo {name!r}")
    start = cli.mark()
    cli.wait_events(start, "GuideStep", count=args.warmup_steps, timeout=args.cell_timeout,
                    abort_on=e2e.GUIDE_FAILURES)
    fake.request("GET", "/fake/frames?clear=1")
    start = cli.mark()
    cli.wait_events(start, "GuideStep", count=args.steps, timeout=args.cell_timeout, abort_on=e2e.GUIDE_FAILURES)
    frames = fake.request("GET", "/fake/frames?clear=1")["frames"]
    steps = cli.events_since(start, ("GuideStep",))[:args.steps]
    lost = cli.events_since(start, ("StarLost",))
    e2e.expect(cli.call("get_app_state") == "Guiding", "not guiding after the cell")
    ra_true = sigma([f["ra_px"] for f in frames])
    dec_true = sigma([f["dec_px"] for f in frames])
    ra_meas = sigma([s.get("RADistanceRaw", 0.0) for s in steps])
    dec_meas = sigma([s.get("DECDistanceRaw", 0.0) for s in steps])
    sim_s = frames[-1]["t"] - frames[0]["t"] if len(frames) > 1 else 0.0
    return {
        "ra": ra_algo, "dec": dec_algo, "frames": len(frames), "steps": len(steps), "star_lost": len(lost),
        "sim_seconds": round(sim_s, 1), "wall_seconds": round(time.time() - t0, 1),
        "ra_true_px": ra_true, "dec_true_px": dec_true, "total_true_px": math.hypot(ra_true, dec_true),
        "ra_measured_px": ra_meas, "dec_measured_px": dec_meas, "total_measured_px": math.hypot(ra_meas, dec_meas),
    }


def run_lane(name, index, args, cells, out):
    """One fake and daemon, a calibration, then the cells. Fills out[name] with {"cells": [...], "error": ...}."""
    lane = LANES[name]
    work = os.path.join(args.work_dir, name)
    os.makedirs(work, exist_ok=True)
    result = {"cells": [], "error": None, "calibration_s": None}
    out[name] = result
    fake = daemon = cli = None
    ctx = {}
    log = open(os.path.join(work, "lane.log"), "w")

    def say(msg):
        line = f"[{name}] {msg}"
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    try:
        fake = e2e.Fake(args.fake_port + index, work, lane["fake_args"])
        ns = types.SimpleNamespace(phd2=args.phd2, instance=args.instance + index)
        daemon = e2e.Daemon(ns, work)
        cli = e2e.Client(4400 + ns.instance - 1, daemon, os.path.join(work, "events.jsonl"), connect_timeout=120)
        ctx["base"], ctx["name"] = e2e.setup_profile(cli, fake)
        cli.call("set_exposure", {"exposure": EXPOSURE_MS})
        lst = fake.telescope("siderealtime")
        fake.slew((lst - 1.0) % 24.0, lane["dec"])  # an hour west of the meridian: no flip
        e2e.loop_and_find_star(cli)
        t = time.time()
        e2e.guide_with_recalibration(cli, 3, args.calibration_timeout)
        result["calibration_s"] = round(time.time() - t, 1)
        say(f"calibrated and guiding in {result['calibration_s']} s")
        for ra_algo, dec_algo in cells:
            if time.time() > args.deadline:
                raise e2e.Failure("out of time before the cell " + f"{ra_algo}/{dec_algo}")
            try:
                cell = run_cell(cli, fake, ra_algo, dec_algo, args)
            except e2e.Failure as e:
                cell = {"ra": ra_algo, "dec": dec_algo, "error": str(e)}
                say(f"FAIL {ra_algo}/{dec_algo}: {e}")
                result["cells"].append(cell)
                # guiding stopped: the rest of the lane cannot run
                if cli.call("get_app_state") != "Guiding":
                    raise
                continue
            result["cells"].append(cell)
            say(f"{ra_algo}/{dec_algo}: true RMS {cell['total_true_px']:.3f} px "
                f"(RA {cell['ra_true_px']:.3f}, Dec {cell['dec_true_px']:.3f}), measured "
                f"{cell['total_measured_px']:.3f} px over {cell['steps']} steps, {cell['sim_seconds']} s simulated, "
                f"{cell['wall_seconds']} s wall")
    except Exception as e:  # the lane failed; its cells so far stand
        result["error"] = f"{type(e).__name__}: {e}"
        say(f"lane failed: {result['error']}")
    finally:
        try:
            if cli is not None and daemon is not None and not daemon.exited():
                if "name" in ctx:
                    e2e.teardown_profile(cli, ctx["base"], ctx["name"])
                e2e.shutdown(cli, daemon)
        except Exception as e:  # noqa: BLE001 - the teardown must not hide the lane's result
            say(f"teardown: {e}")
        finally:
            if cli is not None:
                cli.close()
            if daemon is not None:
                daemon.kill()
                reports = daemon.sanitizer_reports()
                if reports:
                    result["error"] = (result["error"] or "") + " sanitizer reports: " + "; ".join(reports[:5])
                e2e.collect_logs(daemon, work)
            if fake is not None:
                fake.stop()
            log.close()


def key(lane, ra, dec):
    return f"{lane},{ra},{dec}"


def markdown(results, args):
    lanes = [n for n in LANES if n in results]
    md = ["### Daemon algorithm slice", "",
          f"phd2.bin against the fake Alpaca physics, {EXPOSURE_MS / 1000:g} s exposures, {args.steps} GuideSteps "
          f"per cell after {args.warmup_steps} to settle in, one calibration per lane. RMS (sigma) in px of the "
          f"fake's sensor ({FAKE_ARCSEC_PER_PX}\"/px): **true total** (RA / Dec) from /fake/frames, then what the "
          "guider measured (seeing included).", "",
          "| RA algo | Dec algo | " + " | ".join(lanes) + " |",
          "|---|---|" + "---|" * len(lanes)]
    by_key = {key(n, c["ra"], c["dec"]): c for n in lanes for c in results[n]["cells"]}
    cells = [c for c in CELLS if any(key(n, *c) in by_key for n in lanes)]
    for ra, dec in cells:
        row = f"| {ra} | {dec} |"
        for n in lanes:
            c = by_key.get(key(n, ra, dec))
            if c is None:
                row += " - |"
            elif c.get("error"):
                row += f" FAIL: {c['error'][:80]} |"
            else:
                row += (f" **{c['total_true_px']:.3f}** ({c['ra_true_px']:.3f} / {c['dec_true_px']:.3f});"
                        f" measured {c['total_measured_px']:.3f} |")
        md.append(row)
    md.append("")
    for n in lanes:
        r = results[n]
        md.append(f"- **{n}**: Dec {LANES[n]['dec']:g}, fake `{' '.join(LANES[n]['fake_args'])}`; calibration "
                  f"{r['calibration_s']} s" + (f"; **lane failed**: {r['error']}" if r["error"] else ""))
    return "\n".join(md) + "\n"


def read_thresholds(path):
    limits = {}
    with open(path, newline="") as f:
        for row in csv.reader(line for line in f if line.strip() and not line.startswith("#")):
            if len(row) != 4:
                raise SystemExit(f"bad threshold row in {path}: {row}")
            limits[key(*row[:3])] = float(row[3])
    return limits


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--phd2", required=True, help="the phd2.bin to run")
    ap.add_argument("--instance", type=int, default=20, help="the first lane's daemon instance; lane i is + i")
    ap.add_argument("--fake-port", type=int, default=11170, help="the first lane's fake port; lane i is + i")
    ap.add_argument("--work-dir", required=True, help="logs go here (emptied first)")
    ap.add_argument("--lanes", default=",".join(LANES), help="comma-separated lanes to run")
    ap.add_argument("--cells", default="", help="comma-separated RA/Dec pairs (e.g. 'ZFilter/Resist Switch'); all by default")
    ap.add_argument("--warmup-steps", type=int, default=15)
    ap.add_argument("--steps", type=int, default=60)
    ap.add_argument("--calibration-timeout", type=float, default=900)
    ap.add_argument("--cell-timeout", type=float, default=600, help="seconds to the GuideSteps a cell waits for")
    ap.add_argument("--timeout", type=float, default=5400, help="the whole run, seconds")
    ap.add_argument("--thresholds", help="CSV of ceilings: lane,ra,dec,max_true_total_px")
    ap.add_argument("--write-thresholds", help="write ceilings of 1.5 x the measured + 0.1 px here")
    ap.add_argument("--markdown", help="write the summary table here")
    ap.add_argument("--json", help="write the results here")
    args = ap.parse_args()
    args.work_dir = os.path.abspath(args.work_dir)
    args.deadline = time.time() + args.timeout

    lanes = [n for n in args.lanes.split(",") if n]
    unknown = [n for n in lanes if n not in LANES]
    if unknown:
        ap.error(f"unknown lanes {unknown}")
    cells = CELLS
    if args.cells:
        wanted = [tuple(c.split("/")) for c in args.cells.split(",") if c]
        bad = [c for c in wanted if c not in CELLS]
        if bad:
            ap.error(f"unknown cells {bad}")
        cells = tuple(wanted)

    shutil.rmtree(args.work_dir, ignore_errors=True)
    os.makedirs(args.work_dir)
    t0 = time.time()
    results = {}
    threads = [threading.Thread(target=run_lane, args=(n, list(LANES).index(n), args, cells, results), daemon=True)
               for n in lanes]
    for th in threads:
        th.start()
    for th in threads:
        th.join(max(1.0, args.deadline + 300 - time.time()))

    md = markdown(results, args)
    print("\n" + md)
    if args.markdown:
        with open(args.markdown, "w") as f:
            f.write(md)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=1)

    failures = []
    for n in lanes:
        r = results.get(n)
        if r is None:
            failures.append(f"{n}: the lane did not finish")
            continue
        if r["error"]:
            failures.append(f"{n}: {r['error']}")
        failures += [f"{n} {c['ra']}/{c['dec']}: {c['error']}" for c in r["cells"] if c.get("error")]
    if args.write_thresholds:
        with open(args.write_thresholds, "w") as f:
            f.write("# The daemon algorithm slice's ceilings (scripts/e2e_algorithm_matrix.py): a cell's true total RMS,\n"
                    "# px of the fake's sensor, may not exceed max_true_total_px. Written by --write-thresholds as\n"
                    "# 1.5 x the measured + 0.1 px, so only a clear regression fails.\n"
                    "# lane,ra,dec,max_true_total_px\n")
            for n in lanes:
                for c in results.get(n, {}).get("cells", []):
                    if not c.get("error"):
                        f.write(f"{n},{c['ra']},{c['dec']},{math.ceil((1.5 * c['total_true_px'] + 0.1) * 100) / 100:.2f}\n")
    if args.thresholds:
        limits = read_thresholds(args.thresholds)
        for n in lanes:
            for c in results.get(n, {}).get("cells", []):
                if c.get("error"):
                    continue
                k = key(n, c["ra"], c["dec"])
                if k not in limits:
                    failures.append(f"{k}: no threshold row")
                elif not c["total_true_px"] <= limits[k]:
                    failures.append(f"{k}: true total RMS {c['total_true_px']:.3f} px over its ceiling {limits[k]:.2f}")
    print(f"e2e_algorithm_matrix: {sum(len(results.get(n, {}).get('cells', [])) for n in lanes)} cells, "
          f"{len(failures)} failures, {time.time() - t0:.0f} s; logs in {args.work_dir}")
    for f in failures:
        print(f"FAIL {f}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
