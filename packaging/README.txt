Jam Sesh {VERSION} - app and plugin for Windows (64-bit)
=====================================================

Jam Sesh lets you play together online with friends. It's also an oscilloscope and sound kit for
guitar, voice or any other input: it turns what you play into a synth (single notes or whole
chords), lets you shape the wave by hand, runs it through an effects rack, and shows the sound as a
scope, 3D views and an orbital cloud. 126 presets included.

What you get:
- Jam Sesh.exe                   the app: no DAW needed, plug in your guitar and play
- Jam Sesh.vst3                  the same as a plugin, for FL Studio and other DAWs
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

Until 1.4.0 the plugin was called Clod Scope. It is the same plugin: your projects load it as
Jam Sesh, the installer removes the old Clod Scope.vst3, and your settings, presets and recordings
move from the "Clod Scope" folders to "Jam Sesh" (see YOUR FILES).


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
             "Lowest delay" picks for you (see SOUND CARD DELAY below). Under the list it shows
             your sound card's delay as its driver reports it (in + out = round trip), the
             dropouts in the last minute and the load. The button turns amber while the app runs
             on "Windows Audio" or DirectSound, which add about 30 ms.
- Input      your guitar or mic plays through the app. Use headphones: through speakers the
             sound feeds back into the mic or pickup.
- In         which inputs of your interface to use, one by one (In 2 = input 2 only). One input
             sits in the middle. Two inputs (a mic and a guitar) are mixed in the middle, or kept
             left and right for one stereo source.
- BPM / Tap  the tempo for the delays, the beat-synced effects and Jam Sesh. In a sesh a friend
             follows the host's tempo.
- Track      load a backing track to play along with (WAV, MP3, FLAC, OGG, AIFF), or drop the file
             on the bar. Play / Stop, Loop and volume. In a sesh it streams to your friends.
- Record     saves what you hear (you, your friends and the track) as a WAV file in
             Documents\Jam Sesh\Recordings
- i          about the app and its licence

The app opens on the Jam Sesh screen, and remembers its window, audio settings, sound and track
between runs.

The first time it starts, the app picks the audio setup with the lowest delay: your interface's
own ASIO driver at 128 samples; without one, "Windows Audio (Low Latency Mode)" at its smallest
buffer; otherwise Windows' default. It never changes a setup you have saved.


SOUND CARD DELAY

You hear yourself late by your sound card's input + output delay. Your friends hear you late by
your input delay, the network and their own output delay. The driver you pick matters most:
- Your interface's own ASIO driver (Focusrite, Behringer, Steinberg, ...): the lowest delay. Start
  at 128 samples, then try 64.
- No ASIO driver: "Windows Audio (Low Latency Mode)", and pick its SMALLEST buffer: it starts at
  10 ms, which throws most of the gain away. Other apps (Discord, the browser) keep their sound.
- "Windows Audio" (shared mode, the plain one) adds about 30 ms. DirectSound is worse.
- "Windows Audio (Exclusive Mode)" is low too, but it takes the device: other apps on that
  headset or interface go silent while Jam Sesh runs.
- Generic ASIO drivers (ASIO4ALL, "FL Studio ASIO", "Generic Low Latency ASIO Driver", Realtek
  ASIO) only after Low Latency Mode, and never when your interface has its own driver.
- Use 48 kHz: Jam Sesh runs at 48 kHz, other rates are converted.
- The Audio button's "Lowest delay" does the above for you: your interface's ASIO driver at 64
  samples, else Low Latency Mode at its smallest buffer. Never exclusive mode.
- Find your lowest buffer: watch "dropouts" under the list. None for a minute? Try the next
  smaller buffer. Crackles or dropouts? Go one step up.
- Many ASIO drivers work with one program at a time. If "Lowest delay" says the driver didn't
  open, close the other program (a DAW) or use Low Latency Mode.
- Laptops: plug in the charger and use the best-performance power mode.


INSTALL THE PLUGIN

The installer does this for you. From the zip:

1. Copy the "Jam Sesh.vst3" folder into

       C:\Program Files\Common Files\VST3

   Windows asks for administrator permission. If "Clod Scope.vst3" (the plugin's name before
   1.4.0) is there too, delete it: it is the same plugin, and your DAW would list it twice.

2. Restart your DAW, or rescan plugins:
   - FL Studio: Options > Manage plugins > Find installed plugins
   - REAPER:    Options > Preferences > Plug-ins > VST > Re-scan

3. Load "Jam Sesh" as an effect on a mixer insert or track.


PLAY YOUR GUITAR OR VOICE THROUGH THE PLUGIN

Put Jam Sesh on a mixer insert or track whose input is your audio interface, turn on input
monitoring, then click the preset name at the top of the plugin and pick a preset. For low delay,
pick your interface's own ASIO driver in FL Studio's Options > Audio settings (not "FL Studio
ASIO" when your interface has its own driver) and a buffer of 64-128 samples. If notes are missed, set Tracking > Range to match your instrument; to play
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

The app opens on the Jam Sesh view; in the plugin, click Jam Sesh in the top bar. One player hosts
a sesh, up to five friends join. Works in the app and in the plugin, and app and plugin players can
jam together.

1. The host clicks Host. Jam Sesh then looks up the host's internet address (it asks a public
   STUN server, Google's or Cloudflare's; nothing else is sent) and asks the router to let friends
   in (UPnP). The line under the code says whether friends over the internet can join. When it's
   done, copy the sesh code and send it to the friends (chat, Discord, ...).
2. Friends paste the sesh code and click Join. Only the host needs an open port; friends can join
   from anywhere.
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

The sesh code holds the host's internet address: only give it to people you want in the jam.

Backing track: in the app, load it on the bar. In FL Studio, put a second Jam Sesh with the role
"Backing track" on a mixer insert that carries ONLY the beat (beat -> that insert -> Master), never
on the Master itself (it would send your guitar twice). With "Sync track" on, the host hears the
track as late as it reaches the friends, so everyone hears it together.

Far apart? Turn on one-bar mode: everyone hears everyone exactly one bar late, in time.

For the lowest delay:
- use a network cable instead of Wi-Fi
- use your interface's ASIO driver with a small buffer (64-128 samples), or Windows Audio (Low
  Latency Mode) at its smallest buffer; not plain "Windows Audio" (see SOUND CARD DELAY)
- leave compression off when your connection can carry it (about 0.8 Mbit/s per player and
  channel); turn it on, or use raw-input mode, when it can't
- close downloads and video streams while you play


YOUR FILES

Presets you save go to     Documents\Jam Sesh\Presets
Recordings go to           Documents\Jam Sesh\Recordings
Settings (theme, your name for the sesh, the app's audio setup and window) are in %APPDATA%\Jam Sesh

Before 1.4.0 these folders were called "Clod Scope". The first time you start the app or open the
plugin's window, Jam Sesh renames them to "Jam Sesh" (nothing is deleted). If a file in them is in
use, it keeps using the old folders and tries again the next time.


UNINSTALL

Installed with the installer: Settings > Apps > Jam Sesh > Uninstall. It removes the app, the
plugin, the shortcuts and the firewall rule.

From the zip: delete the folder, and
C:\Program Files\Common Files\VST3\Jam Sesh.vst3 if you installed the plugin.

Either way your settings stay in %APPDATA%\Jam Sesh, and your presets and recordings in
Documents\Jam Sesh: delete those too if you want them gone.


FREE SOFTWARE

Jam Sesh: Copyright (C) 2026 Clod Codes

It is free software: you can share it and change it under the terms of the GNU General Public
License version 3 (LICENSE.txt). It comes with NO WARRANTY, to the extent permitted by law.

The complete source code is in JamSesh-{VERSION}-Source.zip, with building instructions
(BUILDING.txt inside). When you pass Jam Sesh on, pass this whole folder or zip on, source code
included: the GPL gives everyone who gets the app the right to its source code.

Built with JUCE (under the AGPLv3), Steinberg's ASIO SDK and VST3 SDK (both under the GPLv3), Opus
(BSD licence) and the New Rocker font (SIL Open Font License). Their licences are in the Licenses
folder; Licenses\JUCE.txt also lists the libraries inside JUCE, whose licences are in the source code.

ASIO is a trademark of Steinberg Media Technologies GmbH, registered in Europe and other countries.
VST is a registered trademark of Steinberg Media Technologies GmbH.
