# Jam Sesh for Mac

### [Download: JamSesh-1.4.0-Mac.zip](https://github.com/Laumerz84/JamSesh/releases/download/v1.4.0/JamSesh-1.4.0-Mac.zip)

The app, plus the plugins as VST3 and Audio Unit. One download works on Apple Silicon (M1 and later) and
Intel Macs, macOS 10.15 Catalina or later. Newer versions are on the
[Releases](https://github.com/Laumerz84/JamSesh/releases) page.

## Install the app

1. Double-click the zip, if your browser hasn't unpacked it already.
2. Drag `Jam Sesh.app` into your Applications folder.
3. Open it. Jam Sesh isn't signed with an Apple Developer ID, so the first time macOS refuses to open it:
   - **macOS 15 Sequoia and later:** double-click Jam Sesh and click **Done**. Then open System Settings >
     Privacy & Security, scroll down to "Jam Sesh was blocked", click **Open Anyway** and confirm.
   - **macOS 14 Sonoma and earlier:** right-click (or Control-click) Jam Sesh in Applications, choose
     **Open**, then click **Open**.
4. macOS asks about two things. Click **Allow** both times:
   - **the microphone:** this covers every input, including your audio interface;
   - **the local network:** Jam Sesh needs it to host or join.

It opens on the Jam Sesh page:

- **Host** a sesh and send your friends the code.
- Or paste a friend's code and **Join**.

Mac and Windows players can jam together.

**Updating:** replace the app with the newer one. Your settings, presets and recordings stay.

## Install the plugins (optional)

1. In Finder choose Go > Go to Folder... (Shift-Command-G), paste `~/Library/Audio/Plug-Ins` and press
   Return.
2. Copy `Jam Sesh.vst3` into the `VST3` folder and `Jam Sesh.component` into `Components`. Make the folders
   if they're missing.
3. Let macOS load them by running this once in Terminal (and again after each update):

       xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"Jam Sesh.vst3"
       xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/"Jam Sesh.component"

4. Restart your DAW, or rescan plugins. In Logic Pro and GarageBand, Jam Sesh is under Audio Units > Clod Codes.

## Good to know

- **Use wired headphones.** Bluetooth headphones and AirPods add 150 ms or more, which is far too late to
  play along.
- **An audio interface** at 64 or 128 samples gives the shortest delay.
- `README-Mac.txt` in the download explains everything else: permissions, the plugins, MIDI, jamming online
  and your files. The source code comes with it (GPLv3).

This is the first Mac version. It's built and checked automatically on GitHub's Mac machines, but it hasn't
been played on a real Mac yet. If something doesn't work, please
[open an issue](https://github.com/Laumerz84/JamSesh/issues).
