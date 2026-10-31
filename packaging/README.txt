Jam Sesh {VERSION} - app and plugin for Windows (64-bit)
=====================================================

Jam Sesh lets you play together online with friends. It's the app of Clod Scope, an oscilloscope and
sound kit for guitar, voice or any other input: it turns what you play into a synth (single notes or
whole chords), lets you shape the wave by hand, runs it through an effects rack, and shows the sound
as a scope, 3D views and an orbital cloud. 126 presets included.

What you get:
- Jam Sesh.exe                   the app: no DAW needed, plug in your guitar and play
- Clod Scope.vst3                the same sound kit as a plugin, for FL Studio and other DAWs
- LICENSE.txt                    the licence: GNU GPL version 3 (see FREE SOFTWARE below)
- Licenses                       the licences of the parts it's built from
- JamSesh-{VERSION}-Source.zip   the source code


INSTALL OR UPDATE

Two downloads, the same Jam Sesh:
- JamSesh-{VERSION}-Setup.exe, the installer: installs the app and the plugin with one admin
  prompt, adds Jam Sesh to the Start menu (and the desktop if you like), can let it through the
  Windows firewall so friends can join when you host, and puts an uninstaller in Settings > Apps.
  To update, run the newer installer: it replaces the old version and asks you to close Jam Sesh
  or your DAW if they are open.
- JamSesh-{VERSION}-Windows.zip: nothing to install. Unzip it anywhere and run Jam Sesh.exe; for
  the plugin, see INSTALL THE PLUGIN below. To update, replace the files with the newer ones.

Either way your settings, presets and recordings stay when you update.


REQUIREMENTS

- Windows 10 or 11, 64-bit
- For the plugin: a DAW that loads VST3 plugins: FL Studio, REAPER, Cubase, Studio One, Bitwig,
  Ableton Live, ...
- The 3D and orbital views need a graphics card with OpenGL 3.2 (any card from the last ten years).
  Without it they show a message and everything else still works.


THE APP (Jam Sesh.exe)

Installed: start Jam Sesh from the Start menu. From the zip: double-click Jam Sesh.exe.

The app and the installer aren't signed, so the first time Windows SmartScreen may say "Windows
protected your PC". Click "More info", then "Run anyway".

The bar under the top of the window:
- Audio      pick your audio interface, its inputs and outputs, sample rate and buffer size.
             Use the ASIO driver of your interface for the lowest delay (64-256 samples).
             No ASIO driver? Use "Windows Audio (Low Latency Mode)" or install ASIO4ALL.
- Input      your guitar or mic plays through the app. Use headphones: through speakers the
             sound feeds back into the mic or pickup.
- BPM / Tap  the tempo for the delays, the beat-synced effects and Jam Sesh. In a Jam Sesh a friend
             follows the host's tempo.
- Track      load a backing track to play along with (WAV, MP3, FLAC, OGG, AIFF), or drop the file
             on the bar. Play / Stop, Loop and volume. In a Jam Sesh it streams to your friends.
- Record     saves what you hear (you, your friends and the track) as a WAV file in
             Documents\Clod Scope\Recordings
- i          about the app and its licence

The app remembers its window, audio settings, sound and track between runs.


INSTALL THE PLUGIN

The installer does this for you. From the zip:

1. Copy the "Clod Scope.vst3" folder into

       C:\Program Files\Common Files\VST3

   Windows asks for administrator permission.

2. Restart your DAW, or rescan plugins:
   - FL Studio: Options > Manage plugins > Find installed plugins
   - REAPER:    Options > Preferences > Plug-ins > VST > Re-scan

3. Load "Clod Scope" as an effect on a mixer insert or track.


PLAY YOUR GUITAR OR VOICE THROUGH THE PLUGIN

Put Clod Scope on a mixer insert or track whose input is your audio interface, turn on input
monitoring, then click the preset name at the top of the plugin and pick a preset. Use an ASIO
driver for low delay. If notes are missed, set Tracking > Range to match your instrument; to play
chords, set Tracking to Chord.


PLAY IT FROM MIDI

The synth also plays MIDI notes (Synth Source set to MIDI or Input + MIDI; most synth presets
already are).
- The app: MIDI keyboards are picked under Audio.
- FL Studio: open the plugin's wrapper settings (the cog at the top left), set a MIDI input port,
  and send to that port from a MIDI Out channel in the Channel Rack.
- REAPER: put MIDI items on the same track, or send MIDI to it from another track.
- Ableton Live doesn't send MIDI to effect plugins, so there the synth follows audio input only.


JAM SESH: PLAY TOGETHER ONLINE

Open the Jam Sesh view (top bar). One player hosts, up to five friends join. Works in the app and
in the plugin, and app and plugin players can jam together.

1. The host clicks Host. Jam Sesh then looks up the host's internet address (it asks a public
   STUN server, Google's or Cloudflare's; nothing else is sent) and asks the router to let friends
   in (UPnP). The line under the code says whether friends over the internet can join. When it's
   done, copy the code and send it to the friends (chat, Discord, ...).
2. Friends paste the code and click Join. Only the host needs an open port; friends can join from
   anywhere.
3. The first time, Windows Firewall asks whether Jam Sesh (or, with the plugin, your DAW) may use
   the network: allow it. If Windows has your home network set to Public, tick Public too.

If the line under the code says friends can't reach you:
- "forward UDP port ...": the router didn't open the port by itself. Turn on UPnP in the router's
  settings, or forward the port to your PC there ("port forwarding").
- "no internet address of its own": your internet provider shares one address between customers
  (common on mobile hotspots and satellite internet), or a second router sits in front of yours.
  Let a friend host instead.
- Or everyone joins the same Tailscale or ZeroTier network (free, no router settings) and the host
  shares the code from there.

The code holds the host's internet address: only give it to people you want in the jam.

Backing track: in the app, load it on the bar. In FL Studio, put a second Clod Scope with the role
"Backing track" on a mixer insert that carries ONLY the beat (beat -> that insert -> Master), never
on the Master itself (it would send your guitar twice). With "Sync track" on, the host hears the
track as late as it reaches the friends, so everyone hears it together.

Far apart? Turn on one-bar mode: everyone hears everyone exactly one bar late, in time.

For the lowest delay:
- use a network cable instead of Wi-Fi
- use ASIO with a small buffer (64-128 samples)
- leave compression off when your connection can carry it (about 0.8 Mbit/s per player and
  channel); turn it on, or use raw-input mode, when it can't
- close downloads and video streams while you play


YOUR FILES

Presets you save go to     Documents\Clod Scope\Presets
Recordings go to           Documents\Clod Scope\Recordings


UNINSTALL

Installed with the installer: Settings > Apps > Jam Sesh > Uninstall. It removes the app, the
plugin, the shortcuts and the firewall rule.

From the zip: delete the folder, and
C:\Program Files\Common Files\VST3\Clod Scope.vst3 if you installed the plugin.

Either way your settings stay in %APPDATA%\Clod Scope, and your presets and recordings in
Documents\Clod Scope: delete those too if you want them gone.


FREE SOFTWARE

Jam Sesh and Clod Scope: Copyright (C) 2026 Clod Codes

They are free software: you can share them and change them under the terms of the GNU General
Public License version 3 (LICENSE.txt). They come with NO WARRANTY, to the extent permitted by law.

The complete source code is in JamSesh-{VERSION}-Source.zip, with building instructions
(BUILDING.txt inside). When you pass Jam Sesh on, pass this whole folder or zip on, source code
included: the GPL gives everyone who gets the app the right to its source code.

Built with JUCE (under the AGPLv3), Steinberg's ASIO SDK and VST3 SDK (both under the GPLv3), Opus
(BSD licence) and the New Rocker font (SIL Open Font License). Their licences are in the Licenses
folder; Licenses\JUCE.txt also lists the libraries inside JUCE, whose licences are in the source code.

ASIO is a trademark of Steinberg Media Technologies GmbH, registered in Europe and other countries.
VST is a registered trademark of Steinberg Media Technologies GmbH.
