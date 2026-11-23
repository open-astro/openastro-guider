# RC51 rig checklist (#142 Tier 4)

What the simulations cannot prove has to be shown on the rig. The in-process closed loop, the
algorithm matrix and the daemon e2e lanes catch regressions in signs, wiring, gain and settle
timing (#142 Tiers 1-4). They cannot show that guiding is good under real seeing, real periodic
error, Dec backlash, the iOptron's own behaviour and RC51's undersampled stars (6.45"/px). This
checklist is the rig pass for each epic step (#115 steps, #142 Tier 2 extractions) before it is
called done. Run it after the build is deployed to RC51 and before the next step merges on top.

The rig: RC51 = 192.168.1.234 (eth0), `astro`/`astro`, Raspberry Pi 4B; AlpacaBridge on port 6800
(mount = device 0, iOptron HAE29C; guide camera = device 1, ZWO ASI120MM Mini); RedCat 51 imaging,
120 mm guide scope at 6.45"/px; the guider as the `openastro-guider` systemd service, JSON-RPC on
:4400 (query it with python3 on the rig, `nc` prints nothing there). Ara drives the night.

Rules for every step:

- **Every new target recalibrates.** Ara's `guide` for a new target sends `"recalibrate": true`.
  Do not skip a calibration to save time, after a flip included.
- **No live parameter changes during a measured run.** `set_algo_param` and `set_algo` while
  guiding are not written to the guide log (see "Replaying the night" below), so a run with one
  cannot be replayed past it. Change parameters, then stop and start guiding, so the next
  "Guiding Begins" header records them.
- Keep the guide log and the debug log of every run (`/var/lib/openastro-guider/PHD2/`), and note
  the deployed version (`get_version`), AlpacaBridge's version and the seeing in the run record.

## 0. Before the night

| Check | How | Pass |
|---|---|---|
| Deployed build | `{"method":"get_version"}` | the commit under test |
| Not hung on a stale lock (#135) | `systemctl status openastro-guider`, then `get_app_state` | `Stopped` answers on :4400 |
| Mount position sane after any power loss | AlpacaBridge `siderealtime` against a computed LST; `:GLS#` status | LST within 1 min; status 7 (at zero) only when at Home. Otherwise `:MSH#` (sensor search), never `:MH#`/Ara Home |
| Pi clock | `date -u` on the rig against the dev VM | within 2 s |
| Guide camera healthy (ASI120 saved settings) | 20 frames: `capture_single_frame` with `{"exposure":1000}` | no 1280 failures |
| Profile is the RC51 one | `get_profile`, `get_profile_setup` | camera `Alpaca Camera [127.0.0.1:6800/1]`, mount `Alpaca Mount [127.0.0.1:6800/0]`, focal length 120 |
| Algorithms and limits as recorded | `get_algo` and `get_algo_param` for each axis; `get_guide_limits`; `get_dec_guide_mode`; `get_backlash_comp` | as in the last baseline night's header (below), or a noted change |

## 1. High-Dec calibration (#141)

The calibration's RA leg must grow its step at high Dec (cos(Dec) shrinks the RA rate) and still
finish, and the measured rates must follow cos(Dec).

1. In Ara, slew to a target at Dec 60-70 (e.g. near Capella or Kochab), an hour or more from the
   meridian so no flip lands in the run.
2. `{"method":"loop"}`, `{"method":"find_star"}`, then `{"method":"guide","params":{"settle":{"pixels":1.5,"time":10,"timeout":90},"recalibrate":true}}`.
3. When `CalibrationComplete` arrives: `{"method":"get_calibration_data","params":{"which":"Mount"}}`.
4. Repeat 1-3 at Dec 20-30.

Pass:

- Both calibrations complete with no `CalibrationFailed`, and no "RA calibration distance" or
  orthogonality alert at high Dec.
- `xRate(high) / xRate(low)` is within 10 % of `cos(Dec high) / cos(Dec low)`; `yRate` agrees
  between the two within 10 %.
- `xAngle` and `yAngle` agree between the two within 3 degrees (same side of the pier), and the
  orthogonality error (`|yAngle - xAngle| - 90`) is under 10 degrees.
- The calibration took under 6 minutes at high Dec (the RA step grows; the 1500 ms step and 25 px
  distance of the profile).

## 2. A real meridian flip

The guider must calibrate again after the flip (Ara's new-target path) and guide with the right
signs on the new side; a stale calibration would drive the RA axis the wrong way.

1. Pick a target 20-30 minutes east of the meridian. Guide it (calibrating) for at least 10
   minutes, with a dither or two.
2. Let Ara flip at the meridian: it stops guiding, slews (the pier side changes), then sends
   `guide` with `"recalibrate": true`.
3. After the new `CalibrationComplete`: `get_calibration_data`. Guide 10 minutes more.

Pass:

- One `StartCalibration` ... `CalibrationComplete` after the flip, then `StartGuiding`,
  `SettleBegin`, exactly one `SettleDone` with `Status` 0 (design/API_CONTRACT.md order).
- `pierSide` in `get_calibration_data` changed; `xAngle` moved by 180 +/- 5 degrees from the
  pre-flip calibration; the RA rate within 5 % of the pre-flip one.
- No runaway: in the first 30 GuideSteps after the settle, `RADistanceRaw` stays under 3 px and
  does not grow frame on frame, and no limit-reached alert.
- The post-flip RMS is within 1.25x the pre-flip RMS (same seeing).

## 3. iOptron limits

The guider's pulse limits and the iOptron's own limits must not fight: a guide pulse must never
be what drives the mount into its meridian or counterweight-up limit, and a clamped pulse must be
reported, not silently lost.

1. `{"method":"get_guide_limits"}`: note `MaxRaDuration` and `MaxDecDuration` (2500 ms on RC51).
2. While guiding, `{"method":"set_guide_limits","params":{"MaxRaDuration":300}}`, then nudge the
   star off with a 3 px dither (`{"method":"dither","params":{"amount":3,"raOnly":true,"settle":{"pixels":1.5,"time":5,"timeout":60}}}`).
   Put the limit back with `set_guide_limits` afterwards.
3. Check AlpacaBridge's mount limits (meridian behaviour, counterweight-angle limit, default 60
   degrees, AlpacaBridge #2395) on the rig's AlpacaBridge settings page, and the HAE29C's
   altitude limit.
4. Guide a target through the iOptron's meridian limit setting only if Ara's flip is turned off
   for this test, with a hand on the power: the mount stops tracking at its limit.

Pass:

- With the 300 ms limit, the GuideSteps of the recovery show `RADuration` 300 and the guide log
  shows the clamp; after the fifth clamped step in a row the limit-reached alert is raised once,
  and not again within 30 s (#153, #164).
- No guide pulse longer than `MaxRaDuration`/`MaxDecDuration` in the guide log outside the
  dither's fast recenter (a recovery move, capped at 25 s only).
- At the mount's limit: AlpacaBridge refuses or the mount stops; the guider reports the failed
  pulses (`PulseGuide command to mount has failed` alert, at most one per 30 s, #188) and stops
  guiding after 10 failures in a row (#213), instead of hanging. Nothing slews.

## 4. Settle times

Ara waits for `SettleDone` after every `guide` and `dither`; the settle has to end, once, in a
time that suits a night of 2-5 minute subs.

1. Guide a target for 10 minutes, then run 10 dithers from Ara's sequence (or
   `{"method":"dither","params":{"amount":3,"raOnly":false,"settle":{"pixels":1.5,"time":10,"timeout":60}}}`
   10 times, waiting for each `SettleDone`).
2. Record each `SettleDone`'s `TotalFrames`, `DroppedFrames` and the time from `SettleBegin`.

Pass:

- Each dither gives exactly one `SettleDone`; at least 9 of 10 with `Status` 0.
- The median settle under 25 s and the worst under 60 s (the timeout) at 1 s exposures, pixels
  1.5, time 10. Compare with the baseline night: the 2026-11-22 settles took 4-12 frames.
- After each settle, the next 10 GuideSteps stay under 1.5 px total.

## 5. RMS against a baseline night

The baseline is the night of 2026-11-22 (guide log `PHD2_GuideLog_2026-11-22_021700.txt` on the
rig; its first 1760 lines are tests/benchmark/fixtures/rc51_guidelog_2026-11-22.txt). Settings:
1 s exposures, Hysteresis on RA (hysteresis 0.10, aggression 0.90, minMove 0.08), Resist Switch on
Dec (aggression 75 %, minMove 0.08, fast switch), max pulses 2500 ms, Dec Auto, no backlash
compensation, multi-star on. Targets around Dec 24. Its RMS (sigma, outside settling) per session:

| Session | Frames | RA | Dec |
|---|---|---|---|
| 04:36:16 | 295 | 0.94" | 0.83" |
| 04:56:03 | 299 | 0.86" | 0.83" |
| 05:08:42 | 958 | 1.19" | 0.92" |
| 06:01:21 | 255 | 0.93" | 0.60" |

1. Use the baseline settings. Guide a target near Dec 20-30, not within an hour of the meridian,
   for at least 30 minutes with dithers as Ara's sequence does.
2. Copy the guide log off the rig and replay it (below).

Pass:

- The session's RMS, RA and Dec each, within 1.3x the baseline sessions' median (RA 0.94", Dec
  0.83") when the seeing is comparable (note FWHM); a worse night is recorded, not failed.
- The replay reproduces the logged corrections (below): every guide distance and pulse, except
  the steps it lists at a threshold or as not sent.

## Replaying the night

`guide_log_replay` (built with the tests, tests/benchmark) reads a guide log and, for each
guiding session, runs the logged offsets through the real wx-free algorithm kernels with the
header's algorithm and parameters:

    cmake --build tmp --target guide_log_replay
    tmp/tests/guide_log_replay --markdown night.md --mismatches PHD2_GuideLog_<date>.txt

It reports how many logged guide distances and pulses the kernels reproduce, the logged RMS, and
the RMS each algorithm (as logged, and each kernel's defaults) is predicted to leave on the
night's motion rebuilt from the log. The prediction assumes the mount moves exactly rate x
duration; it ranks algorithms on the night's sky rather than promising an RMS.

On the four RC51 guide logs with guiding (2026-10-04, 11-08, 11-18, 11-22), every pulse and
every guide distance is reproduced, except in sessions where a parameter was changed over
JSON-RPC while guiding: the log does not record `set_algo_param` (the GUI's parameter-change
lines went with the GUI), so the replay runs with the header's value after the change. A session
that does not reproduce is either such a change or a real difference between the kernels and
what ran on the rig; check the debug log's `GuideAlgorithm...Result()` lines for the frame the
replay names.

## Run record

For each step, record in the epic step's PR or issue: the date, the versions (guider, Ara,
AlpacaBridge), the targets and Decs, the seeing, the RPC results named above, the pass or fail of
each criterion, and the guide and debug logs (attached, or their path on the rig).
