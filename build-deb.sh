#!/usr/bin/env bash
# PHD2 .deb Build Script for Debian 13 Trixie / Raspberry Pi OS Trixie
#
# Supported host architecture: arm64 only (Raspberry Pi 4/5, or Pi 3 on a
# 64-bit OS). amd64, 32-bit ARM (armhf), and i386 are not supported.
#
# Builds PHD2 and creates a .deb for arm64:
# - arm64: produces openastro-phd2-<ver>-arm64.deb
#
# Equipment is reached over ASCOM Alpaca only; there is no INDI/libindi build
# dependency (dropped in Phase 3).

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
err()   { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }
step()  { echo -e "${CYAN}[STEP]${NC} $*"; }

# debian/changelog is a generated build artifact (gitignored), derived from
# the root CHANGELOG.md so release history is maintained in exactly one place.
sync_debian_changelog() {
    local root_changelog="${ROOT_DIR}/CHANGELOG.md"
    step "Generating debian/changelog from version.md + CHANGELOG.md..."
    if [[ ! -f "$root_changelog" ]]; then
        err "Cannot generate debian/changelog: missing $root_changelog"
    fi
    if ! command -v python3 &>/dev/null; then
        err "python3 not found; it is required to generate debian/changelog"
    fi

    local maint_name maint_email
    maint_name=$(git config --get user.name || true)
    maint_email=$(git config --get user.email || true)
    [[ -n "$maint_name" ]] || maint_name="OpenAstro PHD2 Team"
    [[ -n "$maint_email" ]] || maint_email="maintainers@openastro.org"

    mkdir -p "${ROOT_DIR}/debian"
    python3 "${ROOT_DIR}/scripts/changelog_to_deb.py" \
        --changelog "$root_changelog" \
        --out "${ROOT_DIR}/debian/changelog" \
        --package openastro-phd2 \
        --version "$FULL_VERSION" \
        --maintainer "${maint_name} <${maint_email}>"
    info "Generated debian/changelog for version ${FULL_VERSION}"
}

# Project root (directory containing this script)
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
cd "$ROOT_DIR"

# ---------------------------------------------------------------------------
# Architecture check: we support arm64 only (Raspberry Pi target).
# amd64, 32-bit ARM (armhf, e.g. Pi Zero / Pi 1 / Pi 2), and i386 are out of scope.
# ---------------------------------------------------------------------------
HOST_ARCH=$(dpkg --print-architecture 2>/dev/null || echo unknown)
case "$HOST_ARCH" in
    arm64) ;;
    armhf|armel)
        err "Unsupported architecture: ${HOST_ARCH}. This fork builds arm64 only. 32-bit ARM hardware (Pi Zero / Pi 1 / Pi 2) is not supported; on a 64-bit-capable Pi (3/4/5) install the 64-bit Raspberry Pi OS instead."
        ;;
    amd64)
        err "Unsupported architecture: ${HOST_ARCH}. This fork builds arm64 only (Raspberry Pi target); amd64 is not supported."
        ;;
    *)
        err "Unsupported architecture: ${HOST_ARCH}. This fork builds arm64 only."
        ;;
esac
info "Building for ${HOST_ARCH}"

# ---------------------------------------------------------------------------
# Extract version from version.md (single source of truth)
# ---------------------------------------------------------------------------
step "Extracting version from version.md..."
VERSION_MD="${ROOT_DIR}/version.md"
if [[ ! -f "$VERSION_MD" ]]; then
    err "Cannot find version.md at: $VERSION_MD"
fi

FULL_VERSION=$(grep -E '^[[:space:]]*[0-9]+\.[0-9]+\.[0-9]+([A-Za-z0-9._-]*)[[:space:]]*$' "$VERSION_MD" | head -1 | sed -E 's/^[[:space:]]*//; s/[[:space:]]*$//')
if [[ -z "$FULL_VERSION" ]]; then
    err "Could not extract version from version.md (expected line like 1.2.3 or 1.2.3rc1)"
fi
info "Detected version: ${FULL_VERSION}"
sync_debian_changelog

# ---------------------------------------------------------------------------
# Build dependencies (from debian/control). Trixie ships wxWidgets 3.2.
# ---------------------------------------------------------------------------
BUILD_DEPS_CORE=(
    build-essential cmake pkg-config debhelper
    libcfitsio-dev libopencv-dev libv4l-dev
    libcurl4-gnutls-dev libeigen3-dev libgtest-dev
    zlib1g-dev
)
BUILD_DEPS_WX=(libwxgtk3.2-dev)

check_deps() {
    local missing=()
    for pkg in "${BUILD_DEPS_CORE[@]}"; do
        dpkg -s "$pkg" &>/dev/null || missing+=("$pkg")
    done
    local has_wx=false
    for pkg in "${BUILD_DEPS_WX[@]}"; do
        dpkg -s "$pkg" &>/dev/null && { has_wx=true; break; }
    done
    $has_wx || missing+=("libwxgtk3.2-dev")

    if [[ ${#missing[@]} -gt 0 ]]; then
        warn "Missing build dependencies: ${missing[*]}"
        echo ""
        echo "Install build deps (Debian 13 Trixie / Raspberry Pi OS Trixie):"
        echo "  sudo apt-get install -y build-essential cmake pkg-config debhelper \\"
        echo "    libwxgtk3.2-dev libcfitsio-dev libopencv-dev libv4l-dev \\"
        echo "    libcurl4-gnutls-dev libeigen3-dev libgtest-dev zlib1g-dev"
        echo ""
        echo "Or run: $0 --install-deps"
        return 1
    fi
    return 0
}

install_deps() {
    step "Installing build dependencies..."
    sudo apt-get update
    sudo apt-get install -y build-essential cmake pkg-config debhelper \
        libwxgtk3.2-dev libcfitsio-dev libopencv-dev libv4l-dev \
        libcurl4-gnutls-dev libeigen3-dev libgtest-dev zlib1g-dev
    info "Build dependencies installed."
}

# ---------------------------------------------------------------------------
# Parse options
# ---------------------------------------------------------------------------
INSTALL_DEPS=false
CLEAN=false
FORCE=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --install-deps) INSTALL_DEPS=true ;;
        --clean)        CLEAN=true ;;
        --force)        FORCE=true ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Build PHD2 and create a .deb package for the current architecture."
            echo "Result: phd2_<version>_<arch>.deb in the project root parent directory."
            echo ""
            echo "Options:"
            echo "  --install-deps   Install build dependencies (sudo apt-get) then exit."
            echo "  --clean          Remove build artifacts before building."
            echo "  --force          Skip the build-dependency check entirely."
            echo "  -h, --help       Show this help."
            echo ""
            echo "Examples:"
            echo "  $0                  # Build .deb (deps must be installed)"
            echo "  $0 --install-deps   # Install deps only"
            echo "  $0 --clean          # Clean and build"
            echo "  $0 --force          # Build anyway (ignore unmet dep check)"
            exit 0
            ;;
        *) err "Unknown option: $1"; ;;
    esac
    shift
done

if "$INSTALL_DEPS"; then
    install_deps
    exit 0
fi

# ---------------------------------------------------------------------------
# Check we're on a Debian-based system
# ---------------------------------------------------------------------------
if ! command -v dpkg-buildpackage &>/dev/null; then
    err "dpkg-buildpackage not found. Install packaging tools: sudo apt-get install dpkg-dev debhelper"
fi

if ! "$FORCE"; then
    if ! check_deps; then
        err "Install dependencies and re-run, or use: $0 --install-deps  (or $0 --force to try anyway)"
    fi
else
    warn "Skipping build dependency check (--force). Build may fail if a build dependency is missing."
fi

# ---------------------------------------------------------------------------
# Build accelerators: ccache + ninja (same setup as AlpacaBridge's
# build_and_run.sh). debian/rules picks both up automatically when present:
# Ninja as the cmake generator and ccache as the compiler launcher. Every
# dpkg-buildpackage run starts from a clean tree, so ccache is what makes
# repeat package builds fast. Both are tiny, stable Debian packages; if one
# is missing and we have sudo+apt we install it, otherwise we just build
# without (Make + plain compiles).
# ---------------------------------------------------------------------------
accel_missing=()
command -v ccache &>/dev/null || accel_missing+=(ccache)
command -v ninja &>/dev/null || accel_missing+=(ninja-build)
if [[ ${#accel_missing[@]} -gt 0 ]]; then
    if command -v sudo &>/dev/null && command -v apt-get &>/dev/null; then
        step "Installing build accelerators: ${accel_missing[*]}"
        sudo apt-get update -qq
        sudo apt-get install -y "${accel_missing[@]}"
    else
        warn "Missing build accelerators (${accel_missing[*]}) and no sudo+apt; building with Make and no compiler cache."
    fi
fi

# ---------------------------------------------------------------------------
# Clean if requested
# ---------------------------------------------------------------------------
if "$CLEAN"; then
    step "Cleaning previous build artifacts..."
    rm -rf debian/phd2 debian/.debhelper debian/files debian/phd2.substvars
    dpkg-buildpackage -T clean 2>/dev/null || true
    info "Clean done."
fi

# ---------------------------------------------------------------------------
# Build .deb (uses debian/rules: cmake with USE_SYSTEM_GTEST=1, OPENSOURCE_ONLY=1)
# ---------------------------------------------------------------------------
step "Building PHD2 .deb package..."
# -us -uc = do not sign source and changes; -d = allow unmet build deps when --force
if "$FORCE"; then
    dpkg-buildpackage -us -uc -b -d
else
    dpkg-buildpackage -us -uc -b
fi

# ---------------------------------------------------------------------------
# Report result and rename to the user-facing convention
# ---------------------------------------------------------------------------
PARENT_DIR="$(dirname "$ROOT_DIR")"
# Filenames mirror the Package: names in debian/control:
#   openastro-phd2  - the real binary package (architecture-specific)
#   phd2-alpaca     - an Architecture: all transitional metadata package that
#                     depends on openastro-phd2, included so apt-managed
#                     upgrades from the old phd2-alpaca name pull in the new
#                     package automatically.
# dpkg emits `<package>_<version>_<arch>.deb` (underscores); we rename to the
# user-facing `<package>-<version>-<arch>.deb` (hyphens) convention shared
# with the macOS .dmg and Windows .exe. Pin to FULL_VERSION + HOST_ARCH for
# the main package so a stale .deb from a previous version (or a different
# arch) sitting in PARENT_DIR doesn't get reported as this run's output;
# the transitional is Architecture: all so it's pinned on FULL_VERSION
# only. Exclude the dbgsym sibling either way.
DEB=$(find "$PARENT_DIR" -maxdepth 1 -name "openastro-phd2_${FULL_VERSION}_${HOST_ARCH}.deb" ! -name "*-dbgsym_*" -type f 2>/dev/null | head -1)
TRANSITIONAL=$(find "$PARENT_DIR" -maxdepth 1 -name "phd2-alpaca_${FULL_VERSION}_all.deb" ! -name "*-dbgsym_*" -type f 2>/dev/null | head -1)
if [[ -n "$DEB" && -f "$DEB" ]]; then
    RENAMED="${PARENT_DIR}/openastro-phd2-${FULL_VERSION}-${HOST_ARCH}.deb"
    mv -f "$DEB" "$RENAMED"
    DEB="$RENAMED"
    if [[ -n "$TRANSITIONAL" && -f "$TRANSITIONAL" ]]; then
        RENAMED_T="${PARENT_DIR}/phd2-alpaca-${FULL_VERSION}-all.deb"
        mv -f "$TRANSITIONAL" "$RENAMED_T"
        TRANSITIONAL="$RENAMED_T"
    fi
    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}.deb built successfully${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo -e "  ${CYAN}$DEB${NC}"
    ls -la "$DEB"
    if [[ -n "$TRANSITIONAL" && -f "$TRANSITIONAL" ]]; then
        echo -e "  ${CYAN}$TRANSITIONAL${NC} (transitional metadata package; optional)"
        ls -la "$TRANSITIONAL"
    fi
    echo ""
    echo "Install with: sudo dpkg -i $DEB"
    echo "  (resolve deps if needed: sudo apt-get install -f)"
    if [[ -n "$TRANSITIONAL" && -f "$TRANSITIONAL" ]]; then
        echo ""
        echo "Optional: also install the transitional package so existing"
        echo "phd2-alpaca users get auto-migrated through apt:"
        echo "  sudo apt install ./$DEB ./$TRANSITIONAL"
    fi
else
    err ".deb not found in $PARENT_DIR (looked for openastro-phd2_${FULL_VERSION}_${HOST_ARCH}.deb)"
fi
