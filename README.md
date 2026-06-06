# OpenAstro Guider

<img src="icons/oa512.png" alt="OpenAstro Logo" width="125">

## Overview

`openastro-guider` is a **Linux-only, headless, Alpaca-only** guiding engine
derived from [PHD2](https://openphdguiding.org/). It is built to run unattended
on a Raspberry Pi (Linux arm64, the same target as AlpacaBridge) and be driven
remotely over PHD2's JSON-RPC event-server API — primarily by
[**OpenAstro ARA**](https://github.com/open-astro) and alongside
[**AlpacaBridge**](https://github.com/open-astro/AlpacaBridge). All equipment is
reached over **ASCOM Alpaca** (network), with built-in simulator and
manual-pointing backends for testing.

This is a *strip + headless-enable* of PHD2, **not** a rewrite: the guiding
engine, calibration math, guide algorithms, dark/defect-map logic, and the
event-server protocol are kept intact. What was removed is everything that
doesn't belong in a Pi-hosted Alpaca daemon — see [Scope](#scope). The full
plan lives in [`design/PHD2_HEADLESS_PLAYBOOK.md`](design/PHD2_HEADLESS_PLAYBOOK.md).

Lineage: PHD2 (Open PHD Guiding) → `open-astro/openastro-phd2` (added Alpaca +
INDI, GUI) → `open-astro/openastro-guider` (this repo: headless, Alpaca-only).
`openastro-phd2` remains the GUI/INDI reference and is the `upstream` git remote.

> **Status:** wxWidgets (wxBase + the existing GUI) is intentionally retained; a
> headless run mode is being added incrementally per the playbook. Until that
> lands, the binary is still the windowed PHD2 application — just Linux- and
> Alpaca-only.

## Important Notice

- This is **not** an official PHD2 release.
- It is an **OpenAstro-maintained** build, not supported by the PHD2 developers
  or community.
- Support requests should be directed to OpenAstro.

## Scope

Equipment — cameras, mounts, and rotators — is reached through **one** transport,
plus test backends:

- **ASCOM Alpaca** (network) — cameras, mounts, rotators. Drivers are
  **user-installed**; no vendor SDKs are bundled. Designed to work alongside
  AlpacaBridge.
- **Simulator** and **manual pointing** — for development and bench testing.

**Removed compared to upstream PHD2:**

- **Platforms:** Windows and macOS. Linux (Debian/Raspberry Pi OS) only.
- **Transports:** INDI (native client) and Windows ASCOM COM — Alpaca is the
  single equipment path.
- **Backends:** all vendor SDK camera backends (ZWO, QHY, SBIG, Altair, ToupTek,
  SVBony, PlayerOne, Moravian, etc.), adaptive optics / step-guiders, on-camera
  ST4, Shoestring/direct-ST4 guide outputs, and auxiliary mounts.

The intentional shape of this fork is that every supported camera/mount/rotator
is a thin protocol shim over an Alpaca driver the user installs and the vendor
maintains — we don't track vendor SDKs. If you need a bundled-SDK or
cross-platform build, use upstream
[OpenPHDGuiding/phd2](https://github.com/OpenPHDGuiding/phd2) or the
[`openastro-phd2`](https://github.com/open-astro/openastro-phd2) GUI fork instead.

## Running

Supported targets: **Debian 13 Trixie** and **Raspberry Pi OS Trixie**, on
amd64 or arm64. 32-bit ARM (armhf) and i386 are not supported; on a
64-bit-capable Pi (3/4/5) install the 64-bit Raspberry Pi OS.

```bash
./run_deb.sh --build           # configure + parallel build
```

Run the binary at `tmp/phd2.bin` (or via the `tmp/phd2` wrapper). The build
needs no INDI, no vendor SDKs, and no network drivers present — Alpaca devices
are discovered/connected at runtime.

## Building Installers

```bash
./build-deb.sh
```

Produces `../openastro-phd2-<version>-<amd64|arm64>.deb` plus a tiny
`../phd2-alpaca-<version>-all.deb` transitional metadata package. The internal
package name is `openastro-phd2` (renamed from `phd2-alpaca` in 2.0.0;
`Conflicts`/`Replaces` metadata lets dpkg handle the transition cleanly).

- **Fresh install:** use the main `.deb`.
- **apt-managed upgrade** from an existing `phd2-alpaca` host:
  `sudo apt install ./openastro-phd2-<version>-<arch>.deb ./phd2-alpaca-<version>-all.deb`
  pulls in the new package and lets apt retire the old name automatically.

**`build-deb.sh` runs the full test suite before packaging. If any test fails,
no `.deb` is produced.** Fix the failing tests, or pass `-DBUILD_TESTING=OFF` at
configure time to drop the test build entirely.

## Testing

The fork ships a unit-test suite under `tests/` that runs alongside the upstream
Gaussian-process tests under `contributions/MPI_IS_gaussian_process/tests/`.
Tests are wired into CMake's `enable_testing()`, so once you've configured a
build (via `run_deb.sh`) you can run them with `ctest`:

```bash
# from the build directory (tmp/ for run_deb.sh)
cd tmp
ctest --output-on-failure                  # all tests
ctest -R test_alpaca_schema -V             # one suite, verbose
ctest -L "Unit tests"                      # by label

# or build & run individual suites directly
cmake --build . --target test_guiding_stats
./tests/test_guiding_stats
```

Currently **12 test executables** — 4 from the upstream Gaussian-process code
plus 8 added by this fork. The fork's suites are read-only and need no devices,
network, or wxWidgets. They cover:

- the JSON parser behind every Alpaca response and inbound event-server RPC;
- the event-server JSON-RPC message schema downstream consumers depend on
  (NINA / KStars / web UI);
- the Alpaca client/discovery JSON contracts;
- the shared `host:port` parsing and dedupe model used by discovery;
- math-twin pinning of the simple guide algorithms;
- the guiding statistics and filters (`guiding_stats`) and the ZFilter design
  math, linked against the real production code;
- the polar-alignment geometry/astrometry kernels (circle fit, CoR / Alt-Az
  decomposition, reference-star projection, J2000 precession).

See [`tests/README.md`](tests/README.md) for architectural notes and what's
deferred.

## License

This project remains under the original PHD2 licensing terms. See
[LICENSE.txt](LICENSE.txt) for details.
