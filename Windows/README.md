# Jam Sesh for Windows

### [Download the installer: JamSesh-1.4.0-Setup.exe](https://github.com/Laumerz84/JamSesh/releases/download/v1.4.0/JamSesh-1.4.0-Setup.exe)

No installer wanted? [JamSesh-1.4.0-Windows.zip](https://github.com/Laumerz84/JamSesh/releases/download/v1.4.0/JamSesh-1.4.0-Windows.zip)
has the same app and plugin. Newer versions are on the [Releases](https://github.com/Laumerz84/JamSesh/releases) page.

Windows 10 or 11, 64-bit.

## Install

1. Run `JamSesh-1.4.0-Setup.exe`. Jam Sesh isn't signed, so Windows may say "Windows protected your PC":
   click **More info**, then **Run anyway**.
2. Choose what you want:
   - the plugin for FL Studio and other DAWs (VST3);
   - a desktop shortcut;
   - a firewall rule, so friends can join when you host.
3. Click **Install** and say yes to the admin prompt.

Start Jam Sesh from the Start menu or the desktop shortcut. It opens on the Jam Sesh page:

- **Host** a sesh and send your friends the code.
- Or paste a friend's code and **Join**.

The first time it starts, choose your audio device (ASIO gives the shortest delay) and your inputs with the
**In** button.

**Updating:** run the newer installer. It replaces the old version and keeps your settings, presets and
recordings.

**Uninstalling:** Settings > Apps > Jam Sesh.

## Without the installer

1. Unzip `JamSesh-1.4.0-Windows.zip` anywhere.
2. Run `Jam Sesh.exe`.
3. For the plugin, copy `Jam Sesh.vst3` into `C:\Program Files\Common Files\VST3`.

The firewall asks the first time you host; allow it on private networks.

## Before 1.4.0 the plugin was called Clod Scope

- The installer removes the old `Clod Scope.vst3`.
- In FL Studio, rescan your plugins, and Jam Sesh shows up in its place. It keeps the same plugin ID, so
  projects that used Clod Scope should open with Jam Sesh.
- Settings, presets and recordings move from the old `Clod Scope` folders to `Jam Sesh` folders the first
  time 1.4.0 starts.

The `README.txt` in the download explains everything else: the views, the synth, the effects and jamming
online. The source code comes with it (GPLv3).
