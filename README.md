# Jam Sesh

Play together online with friends. Jam Sesh is the app of **Clod Scope**, an oscilloscope and sound kit for
guitar, voice or any other input: it turns what you play into a synth (single notes or whole chords), lets you
shape the wave by hand, runs it through an effects rack, and shows the sound as a scope, 3D views and an orbital
cloud. 126 presets and 13 themes.

- **Jam Sesh.exe**: the standalone app for Windows, no DAW needed
- **Clod Scope.vst3**: the same sound kit as a VST3 plugin, for FL Studio and other DAWs

Get the latest build from [Releases](../../releases): unzip it and run `Jam Sesh.exe`. The README.txt in the
zip explains the app, the plugin and how to jam online.

## Building

On Windows, with Visual Studio 2022 or its Build Tools ("Desktop development with C++"), in an
"x64 Native Tools Command Prompt for VS 2022":

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build

The app is then `build/ClodScope_artefacts/Release/Standalone/Jam Sesh.exe` and the plugin
`build/ClodScope_artefacts/Release/VST3/Clod Scope.vst3`. `package.bat` builds both and packages them, with
their source code, into `dist/`. More in [packaging/BUILDING.txt](packaging/BUILDING.txt).

## Licence

Copyright (C) 2026 Clod Codes. Free software under the GNU General Public License version 3
([LICENSE.txt](LICENSE.txt)).

Built with JUCE 8.0.4 (AGPLv3, with one change marked "Clod Scope patch"), Steinberg's ASIO and VST3 SDKs (under
their GPLv3 option), Opus (BSD licence) and the New Rocker font (SIL Open Font License).

ASIO is a trademark of Steinberg Media Technologies GmbH, registered in Europe and other countries. VST is a
registered trademark of Steinberg Media Technologies GmbH.
