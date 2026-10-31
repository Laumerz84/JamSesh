# Jam Sesh

Play together online with friends. Jam Sesh is also an oscilloscope and sound kit for guitar, voice or any
other input: it turns what you play into a synth (single notes or whole chords), lets you shape the wave by
hand, runs it through an effects rack, and shows the sound as a scope, 3D views and an orbital cloud. 126
presets and 13 themes.

## Download

- **[Windows](Windows/)**: the installer or a zip. You get the app (`Jam Sesh.exe`) and the VST3 plugin for FL
  Studio and other DAWs.
- **[Mac](Mac/)**: the app (`Jam Sesh.app`) and the plugin as VST3 and Audio Unit, for Apple Silicon and Intel
  Macs.

Each folder has the download link and the install steps. Every version is on the [Releases](../../releases)
page. Mac and Windows players can jam together. Before 1.4.0 the plugin was called Clod Scope.

## Building

On Windows, with Visual Studio 2022 or its Build Tools ("Desktop development with C++"), in an
"x64 Native Tools Command Prompt for VS 2022":

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build

The app is then `build/ClodScope_artefacts/Release/Standalone/Jam Sesh.exe` and the plugin
`build/ClodScope_artefacts/Release/VST3/Jam Sesh.vst3`. `package.bat` builds both and packages them, with
their source code, into `dist/`.

On macOS, with Xcode or its Command Line Tools, CMake and Ninja (`brew install cmake ninja`):

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
    cmake --build build --target ClodScope_Standalone ClodScope_VST3 ClodScope_AU

This gives `Jam Sesh.app`, `Jam Sesh.vst3` and `Jam Sesh.component` under `build/ClodScope_artefacts/Release/`.
`packaging/mac/make-dist.sh` packages them, as the [Mac build on GitHub](.github/workflows/build-mac.yml) does.
More in [packaging/BUILDING.txt](packaging/BUILDING.txt).

## Licence

Copyright (C) 2026 Clod Codes. Free software under the GNU General Public License version 3
([LICENSE.txt](LICENSE.txt)).

Built with JUCE 8.0.4 (AGPLv3, with one change marked "Clod Scope patch"), Steinberg's VST3 SDK and, on Windows,
its ASIO SDK (both under their GPLv3 option), Opus (BSD licence) and the New Rocker font (SIL Open Font License).

ASIO is a trademark of Steinberg Media Technologies GmbH, registered in Europe and other countries. VST is a
registered trademark of Steinberg Media Technologies GmbH. Mac and macOS are trademarks of Apple Inc.
