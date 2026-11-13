#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "This script builds the macOS VST3. Run it on a Mac with Xcode Command Line Tools and CMake."
  exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "CMake 3.22 or newer is required. Install CMake, then run this script again."
  exit 1
fi

if ! xcode-select -p >/dev/null 2>&1; then
  echo "Xcode Command Line Tools are required. Run xcode-select --install first."
  exit 1
fi

sculpt_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sculpt_build="${SCULPT_BASS_BUILD_DIR:-${sculpt_root}/build-macos}"
sculpt_dist="${SCULPT_BASS_DIST_DIR:-${sculpt_root}/dist}"
sculpt_jobs="${SCULPT_BASS_BUILD_JOBS:-4}"

configure_args=(
  -S "$sculpt_root"
  -B "$sculpt_build"
  -G "Unix Makefiles"
  -DCMAKE_BUILD_TYPE=Release
  '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64'
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
)
if [[ -n "${SCULPT_JUCE_SOURCE_DIR:-}" ]]; then
  configure_args+=("-DSCULPT_JUCE_SOURCE_DIR=${SCULPT_JUCE_SOURCE_DIR}")
fi

cmake "${configure_args[@]}"
cmake --build "$sculpt_build" --config Release --target SculptBass_VST3 --parallel "$sculpt_jobs"

sculpt_bundle="${sculpt_build}/SculptBass_artefacts/Release/VST3/Sculpt Bass.vst3"
sculpt_binary="${sculpt_bundle}/Contents/MacOS/Sculpt Bass"
if [[ ! -d "$sculpt_bundle" || ! -f "$sculpt_binary" ]]; then
  echo "Build finished without the expected VST3 bundle: $sculpt_bundle"
  exit 1
fi

lipo "$sculpt_binary" -verify_arch arm64 x86_64
plutil -lint "${sculpt_bundle}/Contents/Info.plist"
codesign --force --deep --sign - "$sculpt_bundle"
codesign --verify --deep --strict --verbose=2 "$sculpt_bundle"

mkdir -p "$sculpt_dist"
sculpt_archive="${sculpt_dist}/Sculpt-Bass-macOS-VST3.zip"
ditto -c -k --sequesterRsrc --keepParent "$sculpt_bundle" "$sculpt_archive"

echo "Built universal VST3 (Apple Silicon + Intel): $sculpt_archive"
echo "The bundle is ad-hoc signed, not notarized. No files have been installed into your plug-in folders."
