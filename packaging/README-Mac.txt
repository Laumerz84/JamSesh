Jam Sesh {VERSION} - app and plugins for macOS
==============================================

Jam Sesh lets you play together online with friends. It's also a sound kit for guitar, voice or any
other input: it turns what you play into a synth (single notes or whole chords), lets you shape the
wave by hand, runs it through an effects rack, and shows the sound as a scope, 3D views and an
orbital cloud. 126 presets included.

What you get:
- Jam Sesh.app                   the app: no DAW needed, plug in your guitar and play
- Jam Sesh.vst3                  the same sound kit as a VST3 plugin (Ableton Live, REAPER, FL Studio,
                                 Bitwig, Cubase, Studio One, ...)
- Jam Sesh.component             the same as an Audio Unit (Logic Pro, GarageBand, MainStage, ...)
- LICENSE.txt                    the licence: GNU GPL version 3 (see FREE SOFTWARE below)
- Licenses                       the licences of the parts it's built from
- JamSesh-{VERSION}-Source.zip   the source code


REQUIREMENTS

- macOS 10.15 Catalina or later, on an Apple Silicon (M1 and later) or Intel Mac
- For the plugins: a DAW that loads VST3 plugins or Audio Units
- The 3D and orbital views use OpenGL 3.2, which every Mac that runs macOS 10.15 has. If it can't
  start, they show a message and everything else still works.


INSTALL THE APP

1. Double-click the zip if your browser hasn't unpacked it already.
2. Drag "Jam Sesh.app" into your Applications folder.

To update, replace it with the newer one. Your settings, presets and recordings stay.


OPEN IT THE FIRST TIME

Jam Sesh isn't signed with an Apple Developer ID, so the first time macOS refuses to open it ("can't
be opened because Apple cannot check it for malicious software", or "Apple could not verify ...").
Once you have allowed it, it opens like any other app.

- macOS 15 Sequoia and later: double-click Jam Sesh and click Done when macOS refuses. Then open
  System Settings > Privacy & Security, scroll down to "Jam Sesh was blocked to protect your Mac",
  click Open Anyway and confirm with your password or Touch ID.
- macOS 14 Sonoma and earlier: right-click (or Control-click) Jam Sesh in Applications, choose Open,
  then click Open in the message. (System Settings > Privacy & Security > Open Anyway works too.)
- Or in Terminal, once:
      xattr -dr com.apple.quarantine "/Applications/Jam Sesh.app"


PERMISSIONS

macOS asks before Jam Sesh may use two things. Click Allow (OK) both times:
- The microphone: asked the first time the app opens your audio input. This covers every input,
  your audio interface's too: without it the app hears silence. To change it later: System
  Settings > Privacy & Security > Microphone.
- The local network (macOS 15 and later): asked when you host or join a jam. Jam Sesh needs it to
  reach friends on your network and to ask your router to let friends in. To change it later:
  System Settings > Privacy & Security > Local Network.
- If the macOS firewall is on, it may ask whether Jam Sesh may accept incoming connections when you
  host: click Allow.

In a DAW the plugin runs inside the DAW, so these questions (and the answers) are the DAW's.


THE APP

The bar under the top of the window:
- Audio      pick your audio interface, its inputs and outputs, sample rate and buffer size.
             "Lowest delay" sets the smallest buffer that works well (64 samples). Under the list
             it shows your sound card's delay (in + out = round trip), the dropouts in the last
             minute and the load.
- Input      your guitar or mic plays through the app. Use headphones: through speakers the
             sound feeds back into the mic or pickup.
- In         which inputs of your interface to use (In 2 = input 2 only). One input sits in the
             middle. Two inputs (a mic and a guitar) are mixed in the middle, or kept left and
             right for one stereo source.
- BPM / Tap  the tempo for the delays, the beat-synced effects and Jam Sesh. In a jam everyone
             follows the host's tempo.
- Track      load a backing track to play along with (WAV, MP3, FLAC, OGG, AIFF), or drop the file
             on the bar. Play / Stop, Loop and volume. In a jam it streams to your friends.
- Record     saves what you hear (you, your friends and the track) as a WAV file in your
             Documents folder
- i          about the app and its licence

The app remembers its window, audio settings, sound and track between runs. The first time it
starts it uses the Mac's current input and output at a buffer of 128 samples (macOS's own audio
system has no slow "shared mode", unlike Windows).


SOUND CARD DELAY

You hear yourself late by your sound card's input + output delay. Your friends hear you late by
your input delay, the network and their own output delay.
- An audio interface (Focusrite, Behringer, Steinberg, ...) plugged in with a cable: the lowest
  delay. Start at 128 samples, then try 64.
- Use wired headphones. Bluetooth headphones and AirPods add a lot of delay (150 ms or more), far
  too much to play along.
- Use 48 kHz: Jam Sesh runs at 48 kHz, other rates are converted.
- Find your lowest buffer: watch "dropouts" under the list. None for a minute? Try the next
  smaller buffer. Crackles or dropouts? Go one step up.
- A MacBook: plug in the charger.


INSTALL THE PLUGINS

1. In Finder choose Go > Go to Folder... (Shift-Command-G), paste

       ~/Library/Audio/Plug-Ins

   and press Return. Copy "Jam Sesh.vst3" into the VST3 folder there and "Jam Sesh.component"
   into the Components folder. (Make the folders if they're missing.)

2. The plugins aren't signed with an Apple Developer ID either: let macOS load them by running this
   in Terminal (Applications > Utilities > Terminal), once after each update:

       xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"Jam Sesh.vst3"
       xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/"Jam Sesh.component"

3. Restart your DAW, or rescan plugins. Logic Pro checks a new Audio Unit when it starts; if it
   says Jam Sesh failed, open Logic Pro > Settings > Plug-in Manager, select Jam Sesh and click
   "Reset & Rescan Selection".

4. Load "Jam Sesh" (by Clod Codes) as an effect on the track with your guitar or mic. In Logic Pro
   and GarageBand it's in a track's Audio FX (Plug-ins) slots, under Audio Units > Clod Codes.

To play through it, the track's input is your audio interface and input monitoring is on. Then
click the preset name at the top of the plugin and pick a preset. If notes are missed, set
Tracking > Range to match your instrument; to play chords, set Tracking to Chord.


PLAY IT FROM MIDI

The synth also plays MIDI notes (Synth Source set to MIDI or Input + MIDI; most synth presets
already are).
- The app: MIDI keyboards are picked under Audio.
- VST3 plugin: REAPER, Bitwig and FL Studio send MIDI to effects (REAPER: put MIDI items on the same
  track). Ableton Live doesn't send MIDI to effect plugins.
- The Audio Unit is an audio effect: Logic Pro and GarageBand send it no MIDI. There the synth
  follows what you play on your instrument.


JAM SESH: PLAY TOGETHER ONLINE

One player hosts, up to five friends join. Works in the app and in the plugins, and Mac and Windows
players, app and plugin, can all jam together.

1. The host clicks Host. Jam Sesh then looks up the host's internet address (it asks a public
   STUN server, Google's or Cloudflare's; nothing else is sent) and asks the router to let friends
   in (UPnP). The line under the code says whether friends over the internet can join. When it's
   done, copy the code and send it to the friends (chat, Discord, ...).
2. Friends paste the code and click Join. Only the host needs an open port; friends can join from
   anywhere.
3. The first time, macOS asks about the local network (see PERMISSIONS): allow it.

If the line under the code says friends can't reach you:
- "forward UDP port ...": the router didn't open the port by itself. Turn on UPnP in the router's
  settings, or forward the port to your Mac there ("port forwarding").
- "no internet address of its own": your internet provider shares one address between customers
  (common on mobile hotspots and satellite internet), or a second router sits in front of yours.
  Let a friend host instead.
- Or everyone joins the same Tailscale or ZeroTier network (free, no router settings) and the host
  shares the code from there.

The code holds the host's internet address: only give it to people you want in the jam.

Far apart? Turn on one-bar mode: everyone hears everyone exactly one bar late, in time.

For the lowest delay:
- use a network cable instead of Wi-Fi
- use an audio interface with a small buffer (64-128 samples) and wired headphones
- leave compression off when your connection can carry it (about 0.8 Mbit/s per player and
  channel); turn it on, or use raw-input mode, when it can't
- close downloads and video streams while you play


UNINSTALL

Drag Jam Sesh from Applications to the Trash, and the plugins from ~/Library/Audio/Plug-Ins/VST3
and ~/Library/Audio/Plug-Ins/Components if you installed them. Your settings, presets and
recordings stay; delete them too if you want them gone.


FREE SOFTWARE

Jam Sesh: Copyright (C) 2026 Clod Codes

It is free software: you can share it and change it under the terms of the GNU General Public
License version 3 (LICENSE.txt). It comes with NO WARRANTY, to the extent permitted by law.

The complete source code is in JamSesh-{VERSION}-Source.zip, with building instructions
(BUILDING.txt inside; on a Mac: packaging/mac/make-dist.sh and .github/workflows/build-mac.yml show
the steps). When you pass Jam Sesh on, pass this whole folder or zip on, source code included: the
GPL gives everyone who gets the app the right to its source code.

Built with JUCE (under the AGPLv3), Steinberg's VST3 SDK (under the GPLv3), Opus (BSD licence) and
the New Rocker font (SIL Open Font License). Their licences are in the Licenses folder;
Licenses/JUCE.txt also lists the libraries inside JUCE, whose licences are in the source code. The
source code also holds Steinberg's ASIO SDK, which only the Windows version uses.

VST is a registered trademark of Steinberg Media Technologies GmbH. Mac, macOS, Logic Pro and
GarageBand are trademarks of Apple Inc.
