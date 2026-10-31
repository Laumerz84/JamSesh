#!/bin/bash
# Packages the built Mac app and plugins with their source code for sharing: the macOS twin of
# packaging/make-dist.ps1 (run by .github/workflows/build-mac.yml after the build; needs macOS):
#   <OutDir>/Jam Sesh <version>/          Jam Sesh.app, Jam Sesh.vst3, Jam Sesh.component (Audio Unit),
#                                         README.txt (packaging/README-Mac.txt), LICENSE.txt, Licenses/ and
#                                         JamSesh-<version>-Source.zip
#   <OutDir>/JamSesh-<version>-Mac.zip    the same folder, zipped with ditto so the bundles stay intact
# The bundles get an ad hoc signature (codesign -s -): Apple Silicon Macs run no unsigned code. There is no
# Apple Developer ID behind Jam Sesh, so nothing is notarised; README-Mac.txt says how to open it the first time.
# Jam Sesh is GPLv3 (LICENSE.txt), so every shared copy carries the source code: the same files as the Windows
# package's source zip (make-dist.ps1's list), and nothing private (never docs/). The version comes from
# project(ClodScope VERSION x.y.z) in CMakeLists.txt, the app's name from CLOD_APP_NAME there.
#   bash packaging/mac/make-dist.sh [BuildDir] [OutDir]     (defaults: build, dist; relative to the repository)
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-build}"
out="${2:-dist}"
[[ "$build" = /* ]] || build="$root/$build"
[[ "$out" = /* ]] || out="$root/$out"

version=$(sed -nE 's/^project\(ClodScope VERSION ([0-9.]+)\).*/\1/p' "$root/CMakeLists.txt")
appName=$(sed -nE 's/^set\(CLOD_APP_NAME "([^"]+)"\).*/\1/p' "$root/CMakeLists.txt")
[ -n "$version" ] || { echo "No project version found in CMakeLists.txt" >&2; exit 1; }
[ -n "$appName" ] || { echo "No app name (CLOD_APP_NAME) found in CMakeLists.txt" >&2; exit 1; }
fileName="${appName// /}"

art="$build/ClodScope_artefacts/Release"
app="$art/Standalone/$appName.app"
vst3="$art/VST3/$appName.vst3"
au="$art/AU/$appName.component"
for b in "$app" "$vst3" "$au"; do
    [ -d "$b" ] || { echo "Build it first: $b is missing" >&2; exit 1; }
done

stage="$out/$appName $version"
zip="$out/$fileName-$version-Mac.zip"
sourceName="$fileName-$version-Source"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# a text file with the version filled in and Unix line endings
text() { sed "s/{VERSION}/$version/g" "$1" | tr -d '\r' > "$2"; }

rm -rf "$stage" "$zip"
mkdir -p "$stage"
ditto "$app" "$stage/$appName.app"
ditto "$vst3" "$stage/$appName.vst3"
ditto "$au" "$stage/$appName.component"
xattr -cr "$stage"   # codesign refuses bundles carrying extended attributes ("detritus")
for b in "$stage/$appName.app" "$stage/$appName.vst3" "$stage/$appName.component"; do
    codesign --force --deep --sign - "$b"
    codesign --verify --deep --strict "$b"
done
text "$root/packaging/README-Mac.txt" "$stage/README.txt"
text "$root/LICENSE.txt" "$stage/LICENSE.txt"

# the licences of the parts it's built from (as on Windows)
mkdir -p "$stage/Licenses"
text "$root/JUCE/LICENSE.md" "$stage/Licenses/JUCE.txt"
text "$root/JUCE/LICENSE-AGPLv3.txt" "$stage/Licenses/JUCE - AGPLv3.txt"
text "$root/third_party/asiosdk/LICENSE.txt" "$stage/Licenses/ASIO SDK.txt"
text "$root/JUCE/modules/juce_audio_processors/format_types/VST3_SDK/LICENSE.txt" "$stage/Licenses/VST3 SDK.txt"
text "$root/third_party/opus/COPYING" "$stage/Licenses/Opus.txt"
text "$root/Source/Resources/Fonts/NewRocker-OFL.txt" "$stage/Licenses/New Rocker font.txt"

# The source code, the same list as make-dist.ps1's: JUCE without its examples and extras (extras/Build builds
# JUCE's juceaide tool), Opus without its neural-network model data (dnn/*_data.c: only DRED, OSCE and deep PLC
# use it, and they're off), the ASIO SDK's common folder, and the test tools without their build folders
sourceItems=(
    "CMakeLists.txt" "LICENSE.txt" "README.md" "build.bat" "package.bat" "install-vst3.ps1"
    "Source" "packaging" ".github/workflows"
    "tools/agent-build.ps1" "tools/compile-one.ps1" "tools/snap" "tools/jam-test" "tools/latency-probe" "tools/icon"
    "tools/plugin-test/CMakeLists.txt" "tools/plugin-test/build.bat" "tools/plugin-test/main.cpp"
    "tools/plugin-test/capture.cpp"
    "third_party/opus"
    "third_party/asiosdk/common" "third_party/asiosdk/LICENSE.txt" "third_party/asiosdk/README.md"
    "third_party/asiosdk/changes.txt"
    "JUCE/CMakeLists.txt" "JUCE/LICENSE.md" "JUCE/LICENSE-AGPLv3.txt" "JUCE/README.md" "JUCE/modules"
    "JUCE/extras/Build")
src="$work/$sourceName"
mkdir -p "$src"
for item in "${sourceItems[@]}"; do
    [ -e "$root/$item" ] || { echo "Source code: $item is missing" >&2; exit 1; }
    mkdir -p "$(dirname "$src/$item")"
    ditto --norsrc "$root/$item" "$src/$item"
done
find "$src/third_party/opus/dnn" -maxdepth 1 -name '*_data.c' -delete
find "$src" -name .DS_Store -delete
text "$root/packaging/BUILDING.txt" "$src/BUILDING.txt"
ditto -c -k --norsrc --keepParent "$src" "$stage/$sourceName.zip"

# the zip holds the "<app> <version>" folder itself, so it unpacks into one folder
ditto -c -k --norsrc --keepParent "$stage" "$zip"

sourceFiles=$(find "$src" -type f | wc -l | tr -d ' ')
echo "Packaged $appName $version for macOS ($(lipo -archs "$app/Contents/MacOS/$appName"))"
echo "  folder: $stage"
echo "  zip:    $zip ($(du -h "$zip" | cut -f1), of which source code $(du -h "$stage/$sourceName.zip" | cut -f1), $sourceFiles files)"
