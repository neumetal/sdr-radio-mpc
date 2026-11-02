#!/usr/bin/env bash
# Build an MPC OS standalone plugin (Gen1 MPC Live/One/X/Key, Force: armhf):
# SDR Radio (the default) or SDR Haze (the radio through the Haze step effects).
#
#   mpc/build.sh [vst | haze]
#
# Output in mpc/dist/: the plugin folder ("SdrLab - VST - SDR Radio/", the one
# that goes in a Synths folder), its plugin-list entry, and the installable zip.
#
# Needs: Zig (cross-compiler, at ../tools/zig/zig or on PATH), python3 with
# Pillow, a Python with Playwright and its Chromium for the skin artwork
# (found on PATH, or $ART_PYTHON), a C compiler for this machine, and an
# mpc-vst-plugins checkout at ../mpc-vst-plugins (or $MPC_VST). This replaces
# mpc-vst-plugins' tools/build_port.sh, which needs Docker: same flags, same
# glibc 2.31 target.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$(cd "$HERE/.." && pwd)"
MV="${MPC_VST:-$(cd "$APP/../mpc-vst-plugins" && pwd)}"
ZIG="${ZIG:-$APP/../tools/zig/zig}"
[ -x "$ZIG" ] || ZIG="$(command -v zig)"

PORT="${1:-vst}"
case "$PORT" in
    vst)  VERSION_FILE="$HERE/VERSION";      ID=sdr-radio
          ABOUT="Live FM, AM and narrow-FM radio from an RTL-SDR dongle: station presets, and band, preset and squelch scanning on the beat" ;;
    haze) VERSION_FILE="$HERE/haze/VERSION"; ID=sdr-haze
          ABOUT="Live radio from an RTL-SDR dongle through a 16-step effects sequencer locked to the MPC's clock: repeat, reverse, tape stop, pitch, filter, phaser, delay, reverb, distortion, mute" ;;
    *) echo "usage: mpc/build.sh [vst | haze]" >&2; exit 1 ;;
esac
[ -d "$HERE/$PORT" ] || { echo "error: mpc/$PORT isn't in this checkout" >&2; exit 1; }
# The GitHub repo the release zip names (its catalog source_repo): mpc/REPO, or $SDR_REPO.
REPO="${SDR_REPO:-$(cat "$HERE/REPO" 2>/dev/null || true)}"
VST="$HERE/$PORT"
GEN="$VST/build"
OBJ="$HERE/obj/$PORT"
DIST="$HERE/dist"
TP="$APP/third_party"

TARGET=(-target arm-linux-gnueabihf.2.31 -mcpu=cortex_a17)   # Gen1 CPU: Rockchip RK3288
COMMON=("${TARGET[@]}" -O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections)

# 0. The MPC lists the desktop parameters by hand; stop if they drifted.
python3 "$APP/tools/check_mpc_sync.py" "mpc/$PORT"

# 1. params.h, skin and plugin-list entry (mpc-vst-plugins' generator; its
#    artwork renderer runs on this machine).
mkdir -p "$GEN" "$OBJ" "$DIST"
cc -O2 -I"$MV/tools/vendor/force-shadow/tools" -o "$GEN/shadow_art" "$MV/tools/shadow_art.c" -lm
# The artwork is drawn by mpc-vst-plugins' browser renderer (vst.json "art":
# "html"), run here with the first Python that has Playwright.
ART_PY="${ART_PYTHON:-}"
if [ -z "$ART_PY" ]; then
    for p in python3 python3.13 python3.12 python3.11 /usr/local/bin/python3.13; do
        if command -v "$p" >/dev/null && "$p" -c 'import playwright, PIL' 2>/dev/null; then ART_PY="$(command -v "$p")"; break; fi
    done
fi
[ -n "$ART_PY" ] || { echo "error: no Python with Playwright and Pillow for the skin artwork (set ART_PYTHON)" >&2; exit 1; }
printf '#!/bin/sh\nexec "%s" "%s" "$@"\n' "$ART_PY" "$MV/tools/html_art.py" > "$GEN/html_art"
chmod +x "$GEN/html_art"
export SHADOW_ART="$GEN/html_art"
rm -rf "$GEN/skin"
# The MPC OS 2.x skin shape, which 3.x also draws, so one build covers both.
SHADOW_SKIN_MPC_OS="${SHADOW_SKIN_MPC_OS:-2}" python3 "$MV/tools/gen_vst.py" "$VST/vst.json"

# 2. Compile. Each source -> its own object, by language.
OBJS=()
compile() {   # compile <source> <flags...>
    local src="$1"; shift
    local rel="${src#$APP/}"
    case "$rel" in /*) rel="ext/$(basename "$(dirname "$rel")")/$(basename "$rel")" ;; esac   # outside the project (the wrapper)
    local o="$OBJ/$(echo "$rel" | tr '/' '_').o"
    case "$src" in
        *.cpp|*.cc) "$ZIG" c++ "${COMMON[@]}" -std=c++17 "$@" -c "$src" -o "$o" ;;
        *)          "$ZIG" cc  "${COMMON[@]}" -std=gnu11 "$@" -c "$src" -o "$o" ;;
    esac
    OBJS+=("$o")
}

# libusb (LGPL-2.1), Linux backend, no udev
USB="$TP/libusb/libusb"
USBF=(-I"$HERE/libusb_config" -I"$USB" -I"$USB/os" -include config.h -w)
for f in core descriptor hotplug io strerror sync os/linux_usbfs os/linux_netlink os/events_posix os/threads_posix; do
    compile "$USB/$f.c" "${USBF[@]}"
done

# librtlsdr (GPL-2.0); detach the kernel DVB driver if it has claimed the dongle
RTL="$TP/rtl-sdr"
RTLF=(-I"$RTL/include" -I"$USB" -Drtlsdr_STATIC -DDETACH_KERNEL_DRIVER=1 -w)
for f in librtlsdr tuner_e4k tuner_fc0012 tuner_fc0013 tuner_fc2580 tuner_r82xx; do
    compile "$RTL/src/$f.c" "${RTLF[@]}"
done

# Radio core + the plugin's engine
APPF=(-I"$APP/Source" -I"$HERE/src" -I"$RTL/include" -I"$MV/wrapper" -Drtlsdr_STATIC -Wall -Wextra -Wno-unused-parameter)
compile "$APP/Source/SdrReceiver.cpp" "${APPF[@]}"
if [ "$PORT" = haze ]; then
    # Haze: our effect chain, with Emilie Gillet's reverb and limiter (MIT, header-only)
    EU="$TP/eurorack"
    compile "$APP/Source/HazeEngine.cpp" -I"$EU" -I"$APP/Source" -DTEST -w
    compile "$HERE/src/sdr_haze_engine.cpp" "${APPF[@]}"
else
    compile "$HERE/src/sdr_mpc_engine.cpp" "${APPF[@]}"
fi

# mpc-vst-plugins' VST2 wrapper (MIT)
compile "$MV/wrapper/vst2_wrap.c" -I"$GEN" -I"$MV/wrapper" -Wall -Wextra -Wno-unused-parameter

# 3. Link. libc++ goes in statically (Zig's), so only glibc is needed on the device.
#    exports.map keeps every symbol but VSTPluginMain private: MPC may load its
#    own libusb, and an exported copy of ours would be interposed by it.
SO="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["so"])' "$VST/vst.json")"
"$ZIG" c++ "${TARGET[@]}" -shared -fPIC -Wl,--no-undefined -Wl,--gc-sections -s \
    -Wl,--version-script="$HERE/exports.map" -Wl,-Bsymbolic \
    "${OBJS[@]}" -lpthread -lm -o "$OBJ/$SO"
python3 "$APP/tools/check_so.py" "$OBJ/$SO"    # ARM hard-float, glibc <= 2.31, one export

# 4. Lay out the plugin folder.
SKIN="$(ls -d "$GEN/skin/"*/ | head -1)"
NAME="$(basename "$SKIN")"
rm -rf "$DIST/$NAME"
cp -R "$SKIN" "$DIST/$NAME"
cp "$OBJ/$SO" "$DIST/$NAME/$SO"
cp "$GEN/pluginlist-entry.xml" "$DIST/$NAME.pluginlist-entry.xml"

# 5. Package as an installable zip (mpc-vst-plugins' release tool: install.sh,
#    INSTALL.md, checksums). License texts ride along in the plugin folder.
LIC="$OBJ/LICENSES"
rm -rf "$LIC"; mkdir -p "$LIC"
cp "$TP/rtl-sdr/COPYING"      "$LIC/librtlsdr-GPL-2.0.txt"
cp "$TP/libusb/COPYING"       "$LIC/libusb-LGPL-2.1.txt"
cp "$MV/LICENSE"              "$LIC/mpc-vst-plugins-MIT.txt"
[ "$PORT" = haze ] && cp "$TP/eurorack/LICENSE" "$LIC/eurorack-MIT.txt"
VERSION="$(cat "$VERSION_FILE")"
python3 "$MV/tools/release.py" --so "$OBJ/$SO" --skin "$GEN/skin/$NAME" --entry "$GEN/pluginlist-entry.xml" \
    --version "$VERSION" --extra "$LIC:LICENSES" --license GPL-2.0-or-later --id "$ID" ${REPO:+--repo "$REPO"} \
    --about "$ABOUT" \
    --requires "An RTL-SDR dongle in the MPC's USB port" -o "$DIST"

echo "built: $DIST/$NAME/$SO"
