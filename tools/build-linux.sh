#!/usr/bin/env bash
# Build a Super Mario 64 DS Recomp Linux x86_64 AppImage.
#
# The current recomp-ui launcher is Windows-only, so this packages the title
# runner directly. Put a legally dumped Super Mario 64 DS .nds beside the
# AppImage; AppRun auto-detects the ROM and falls back to FreeBIOS +
# generated firmware when no retail BIOS folder exists.
set -euo pipefail

APP_NAME="SuperMario64DSRecomp"
TITLE_TARGET="sm64ds_generate_banks"
RUNNER_NAME="nds_runner"
VERSION="0.0.3"
JOBS="$(nproc 2>/dev/null || echo 4)"
DO_PACKAGE=1

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$REPO/release-linux"

while [ $# -gt 0 ]; do
  case "$1" in
    --version) VERSION="$2"; shift 2;;
    --jobs) JOBS="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --no-package) DO_PACKAGE=0; shift;;
    -h|--help)
      sed -n '2,14p' "$0"
      exit 0
      ;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

GAME_BUILD="$REPO/build-linux-release"
RUNNER_BUILD="$REPO/ndsrecomp/runner/build-sm64-linux-release"
TITLE_BANK_DIR="$REPO/generated/recomp"

cd "$REPO"
test -f "$REPO/ndsrecomp/recompiler/CMakeLists.txt" || {
  echo "ERROR: ndsrecomp submodule is not initialized." >&2
  exit 1
}
test -f "$REPO/Super Mario 64 DS.nds" || {
  echo "ERROR: verified Super Mario 64 DS ROM is missing from the repo root." >&2
  exit 1
}
FREEBIOS9="$REPO/ndsrecomp/third_party/freebios/drastic_bios_arm9.bin"
FREEBIOS7="$REPO/ndsrecomp/third_party/freebios/drastic_bios_arm7.bin"
test -f "$FREEBIOS9" && test -f "$FREEBIOS7" || {
  echo "ERROR: FreeBIOS submodule is missing. Run: git -C ndsrecomp submodule update --init third_party/freebios" >&2
  exit 1
}

echo "[1/4] configure title banks"
cmake -S "$REPO" -B "$GAME_BUILD" -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNDSRECOMP_ROOT="$REPO/ndsrecomp" \
  -DSM64DS_BUILD_LAUNCHER=OFF
echo "[2/4] build title banks"
cmake --build "$GAME_BUILD" --target "$TITLE_TARGET" -j"$JOBS"

RECOMPILER="$GAME_BUILD/ndsrecomp-recompiler/nds_recompile"
FRAMEWORK_GENERATED="$RUNNER_BUILD/generated"
mkdir -p "$FRAMEWORK_GENERATED"
"$RECOMPILER" \
  --config "$REPO/ndsrecomp/bios/biosnds9.toml" \
  --bin "$REPO/ndsrecomp/bios/biosnds9.rom" \
  --out "$FRAMEWORK_GENERATED" \
  --bank arm9_bios
"$RECOMPILER" \
  --config "$REPO/ndsrecomp/bios/biosnds7.toml" \
  --bin "$REPO/ndsrecomp/bios/biosnds7.rom" \
  --out "$FRAMEWORK_GENERATED" \
  --bank arm7_bios
"$RECOMPILER" \
  --config "$REPO/ndsrecomp/bios/freebios9.toml" \
  --bin "$FREEBIOS9" \
  --out "$FRAMEWORK_GENERATED" \
  --bank freebios_arm9
"$RECOMPILER" \
  --config "$REPO/ndsrecomp/bios/freebios7.toml" \
  --bin "$FREEBIOS7" \
  --out "$FRAMEWORK_GENERATED" \
  --bank freebios_arm7

echo "[3/4] configure runner"
cmake -S "$REPO/ndsrecomp/runner" -B "$RUNNER_BUILD" -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNDS_BOOTSTRAP_FIRMWARE=ON \
  -DNDS_3D_GEOMETRY=NATIVE \
  -DNDS_RECOMP_UI_ROOT="$REPO/third_party/recomp-ui" \
  -DCMAKE_PROJECT_nds_runner_INCLUDE="$REPO/cmake/runner-ui.cmake" \
  -DNDS_GENERATED_DIR="$FRAMEWORK_GENERATED" \
  -DNDS_SM64DS_BANK_DIR="$TITLE_BANK_DIR"
echo "      build runner"
cmake --build "$RUNNER_BUILD" --target nds_runner -j"$JOBS"

if [ "$DO_PACKAGE" = "0" ]; then
  echo "done: $RUNNER_BUILD/$RUNNER_NAME"
  exit 0
fi

BIN="$RUNNER_BUILD/$RUNNER_NAME"
test -f "$BIN" || { echo "ERROR: runner not built: $BIN" >&2; exit 1; }
for bank in sm64ds_arm9 sm64ds_arm7 sm64ds_arm7_ram sm64ds_arm9_ram sm64ds_arm9_ram_gameplay; do
  strings "$BIN" | grep -q "$bank" || {
    echo "ERROR: runner does not contain expected title bank: $bank" >&2
    exit 1
  }
done

LINUXDEPLOY_URL=https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
LINUXDEPLOY_SHA=421ca71d5c69ea97c6309276232990d43df1dcece0edfaa26bbf926ff96ed12e
APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
APPIMAGETOOL_SHA=a6d71e2b6cd66f8e8d16c37ad164658985e0cf5fcaa950c90a482890cb9d13e0

TOOLS_DIR="$RUNNER_BUILD/appimage-tools"
mkdir -p "$TOOLS_DIR" "$OUT"
fetch_tool() {
  local url="$1" sha="$2" dest="$3"
  if [ ! -f "$dest" ] || [ "$(sha256sum "$dest" | awk '{print $1}')" != "$sha" ]; then
    curl -fL --retry 3 "$url" -o "$dest.tmp"
    printf '%s  %s\n' "$sha" "$dest.tmp" | sha256sum -c - >/dev/null
    mv "$dest.tmp" "$dest"
  fi
  chmod 0755 "$dest"
}
LINUXDEPLOY_BIN="$TOOLS_DIR/linuxdeploy-x86_64.AppImage"
APPIMAGETOOL_BIN="$TOOLS_DIR/appimagetool-x86_64.AppImage"
fetch_tool "$LINUXDEPLOY_URL" "$LINUXDEPLOY_SHA" "$LINUXDEPLOY_BIN"
fetch_tool "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA" "$APPIMAGETOOL_BIN"

WORK="$(mktemp -d)"
trap 'chmod -R u+w "$WORK" 2>/dev/null || true; rm -rf "$WORK"' EXIT
APPDIR="$WORK/AppDir"
mkdir -p "$APPDIR/usr/bin/bios" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"

cp "$BIN" "$APPDIR/usr/bin/$RUNNER_NAME"
cp "$REPO/game.toml" "$APPDIR/usr/bin/game.toml"
cp "$REPO/README.md" "$APPDIR/usr/bin/README.md"
cp "$REPO/LICENSE" "$APPDIR/usr/bin/LICENSE"
# The runner links vendored melonDS, so the shipped executable is a
# GPL-3.0-or-later combined work and the release must carry the GPL text.
cp "$REPO/LICENSE-GPL" "$APPDIR/usr/bin/LICENSE-GPL"
cp "$REPO/packaging/BIOS_README.txt" "$APPDIR/usr/bin/bios/README.txt"

python3 - "$APPDIR/usr/share/icons/hicolor/256x256/apps/$APP_NAME.png" <<'PY'
import struct, sys, zlib
out = sys.argv[1]
n = 256
raw = b''.join(bytes([0]) + bytes([49, 154, 72]) * n for _ in range(n))
def chunk(t, d):
    c = t + d
    return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
open(out, "wb").write(png)
PY
cat > "$APPDIR/usr/share/applications/$APP_NAME.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Super Mario 64 DS Recomp
Exec=$RUNNER_NAME
Icon=$APP_NAME
Categories=Game;
Terminal=false
EOF

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib:${LD_LIBRARY_PATH:-}"
export SDL_JOYSTICK_HIDAPI_STEAM=1
export SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD=1
SELF="${APPIMAGE:-$0}"
RUNDIR="$(dirname "$(readlink -f "$SELF")")"
mkdir -p "$RUNDIR/bios" 2>/dev/null || true
if [ ! -f "$RUNDIR/bios/README.txt" ] && [ -f "$HERE/usr/bin/bios/README.txt" ]; then
  cp "$HERE/usr/bin/bios/README.txt" "$RUNDIR/bios/README.txt" 2>/dev/null || true
fi
ROM=""
for f in "$RUNDIR"/*.nds "$RUNDIR"/*.NDS; do
  [ -e "$f" ] && ROM="$f" && break
done
MODE=""
if [ ! -f "$RUNDIR/bios/biosnds9.rom" ] || [ ! -f "$RUNDIR/bios/biosnds7.rom" ] || \
   [ ! -f "$RUNDIR/bios/firmware.bin" ]; then
  MODE="--freebios --generated-firmware"
fi
cd "$RUNDIR" 2>/dev/null || true
if [ "$#" -eq 0 ]; then
  if [ -n "$ROM" ]; then
    exec "$HERE/usr/bin/nds_runner" "$RUNDIR/bios" --interactive \
      --rom "$ROM" --config "$HERE/usr/bin/game.toml" \
      --screen-layout separate --adaptive-widescreen top \
      --boot direct $MODE
  fi
  exec "$HERE/usr/bin/nds_runner" "$RUNDIR/bios" --interactive \
    --config "$HERE/usr/bin/game.toml" --screen-layout separate \
    --adaptive-widescreen top --boot direct $MODE
fi
exec "$HERE/usr/bin/nds_runner" "$@"
EOF
chmod +x "$APPDIR/AppRun" "$APPDIR/usr/bin/$RUNNER_NAME"

echo "[4/4] package AppImage"
"$LINUXDEPLOY_BIN" --appimage-extract-and-run --appdir "$APPDIR" --executable "$APPDIR/usr/bin/$RUNNER_NAME" \
  --desktop-file "$APPDIR/usr/share/applications/$APP_NAME.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/$APP_NAME.png" >/dev/null

APP="$OUT/$APP_NAME-linux-v$VERSION-x86_64.AppImage"
rm -f "$APP"
ARCH=x86_64 "$APPIMAGETOOL_BIN" --appimage-extract-and-run "$APPDIR" "$APP" >/dev/null
chmod +x "$APP"
bash "$REPO/tools/test_appimage_layout.sh" "$APPDIR"
sha256sum "$APP"
