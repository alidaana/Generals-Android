#!/usr/bin/env bash
# Fetch a prebuilt Mesa Turnip Vulkan driver package (adrenotools ADPKG format)
# for Adreno 6xx/7xx/8xx (arm64). This is the analogue of fetch-moltenvk.sh for iOS:
# it downloads a driver blob at build time so nothing binary is committed to git.
#
# Provenance: K11MCH1/AdrenoToolsDrivers — the reference Turnip build repo cited in
# the Phase 0 renderer research (§2.2). Turnip is Mesa's open-source Vulkan 1.3
# driver for Qualcomm Adreno (freedreno); MIT-licensed, so bundling is permitted.
# The Adreno 650 (a6xx) is Turnip's rock-solid tier.
#
# Output: build/android-turnip/pkg/  containing meta.json + the driver, always
# available as vulkan.ad07xx.so (the name the packaging step and runtime expect)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
OUT_DIR="${PROJECT_ROOT}/build/android-turnip"
PKG_DIR="${OUT_DIR}/pkg"

# Pinned Turnip release. Override with TURNIP_URL to try a different build.
#
# S25 branch: Adreno 8xx (Snapdragon 8 Elite: A830 in the Galaxy S25 series, A840)
# needs a Turnip with gen8 support, which the K11MCH1 v25.3.0 R11 build lacks: on
# A830 it enumerates 0 physical devices -> DXVK "No adapters found" -> crash at
# boot. whitebelyash's "Stable Turnip v2" (Mesa 26.2.3, turnip/26.2 branch of
# whitebelyash/mesa-unified) supports A8xx (840/830/829/825/812/810) plus every
# A6xx/A7xx GPU upstream Turnip supports, so it also keeps older Adreno working.
# The previous pin, for reference:
#   TURNIP_TAG=v25.3.0-rc.11 TURNIP_ASSET=Turnip_v25.3.0_R11.zip
#   TURNIP_URL=https://github.com/K11MCH1/AdrenoToolsDrivers/releases/download/v25.3.0-rc.11/Turnip_v25.3.0_R11.zip
TURNIP_TAG="${TURNIP_TAG:-stu_v2}"
TURNIP_ASSET="${TURNIP_ASSET:-stable-turnip-V2.zip}"
TURNIP_URL="${TURNIP_URL:-https://github.com/whitebelyash/AdrenoToolsDrivers/releases/download/${TURNIP_TAG}/${TURNIP_ASSET}}"

mkdir -p "${OUT_DIR}"
ZIP="${OUT_DIR}/$(basename "${TURNIP_ASSET}")"

if [[ ! -f "${ZIP}" ]]; then
    echo "==> Downloading Turnip: ${TURNIP_URL}"
    curl -fL "${TURNIP_URL}" -o "${ZIP}"
else
    echo "==> Using cached ${ZIP}"
fi
echo "==> sha256: $(sha256sum "${ZIP}" | cut -d' ' -f1)  $(basename "${ZIP}")"

rm -rf "${PKG_DIR}"; mkdir -p "${PKG_DIR}"
unzip -o -j "${ZIP}" -d "${PKG_DIR}" >/dev/null

# The ADPKG carries meta.json + the driver .so. libraryName in meta.json names it.
DRIVER_SO="$(find "${PKG_DIR}" -name '*.so' | head -1)"
[[ -n "${DRIVER_SO}" ]] || { echo "ERROR: no .so in Turnip package" >&2; exit 1; }
[[ -f "${PKG_DIR}/meta.json" ]] || { echo "ERROR: no meta.json in Turnip package" >&2; exit 1; }

# Artifact verification (never trust the filename): arm64 + actually Turnip/freedreno.
readelf -h "${DRIVER_SO}" | grep -q AArch64 || { echo "ERROR: driver .so is not arm64" >&2; exit 1; }
# Capture strings BEFORE grepping: `grep -q` exits early on match and SIGPIPEs
# `strings`, which under `set -o pipefail` would fail the whole pipeline even
# though the match was found (same trap documented in package-android-zh.sh).
DRIVER_STRINGS="$(strings "${DRIVER_SO}")"
grep -qiE 'turnip|freedreno|mesa' <<<"${DRIVER_STRINGS}" || {
    echo "ERROR: driver .so does not look like Mesa Turnip/freedreno" >&2; exit 1; }

# package-android-zh.sh embeds pkg/vulkan.ad07xx.so, and SDL3Main.cpp stages and
# dlopens the driver under that name. K11MCH1 packages already use it; other
# ADPKGs (whitebelyash's included) ship libvulkan_freedreno.so — normalize.
if [[ "$(basename "${DRIVER_SO}")" != "vulkan.ad07xx.so" ]]; then
    cp "${DRIVER_SO}" "${PKG_DIR}/vulkan.ad07xx.so"
    echo "==> normalized $(basename "${DRIVER_SO}") -> vulkan.ad07xx.so"
fi

echo "==> Turnip package ready in ${PKG_DIR}:"
ls -1 "${PKG_DIR}"
echo "==> meta.json:"; cat "${PKG_DIR}/meta.json"; echo
echo "==> driver: $(basename "${DRIVER_SO}") ($(readelf -h "${DRIVER_SO}" | awk '/Machine/{print $2,$3}'))"
