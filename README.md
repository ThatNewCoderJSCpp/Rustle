# Rustle

A soundboard and voice changer for Linux (and Windows and macOS), built with [Fizmo](https://github.com/ThatNewCoderJSCpp/Fizmo), [Grove](https://github.com/ThatNewCoderJSCpp/Grove) and [Walnut](https://github.com/ThatNewCoderJSCpp/Walnut).

Rustle makes a virtual microphone called **Rustle Mic**. Pick it as your microphone in Discord, OBS, TeamSpeak or a game, and the people on the other end hear your voice through Rustle's voice changer, plus any sound you play from the soundboard. You hear the sounds too, and can hear your own changed voice if you like.

## What it does

- **Sounds**: add one or more folders and browse them the way they are on disk. Folders show up as cards you click into; a breadcrumb bar and the back button take you up again. Every sound is a tile (or a row, in list view) with its length, its key and a progress bar while it plays. Click to play, click again to stop. Search looks through the whole library at once, and there's a Favorites view, a Most played view and sorting by name, length, newest or most played. Rustle plays WAV, FLAC, MP3, Ogg Vorbis, Opus, AAC and M4A files, and the sound of MP4, MOV, MKV and WebM videos. New and changed files are picked up on their own every few seconds (or when you press the button, if you'd rather).
- **Sound editing**: every sound has its own speed (faster and higher, like a record), stretch (faster without changing the pitch), pitch, reverse, reverb with a room size, volume, start and end points, fade in and out, and loop. Edits never touch the file; Rustle keeps them in its settings and renders the sound when it's played (and caches the result).
- **Voice**: 22 effects in one panel, all off until you touch them: noise gate, noise reduction, low cut, pitch (with fine tune, formant keeping and two methods), formant, bitcrush, voice tone, a vocoder "synth voice", robot (ring modulation), distortion, flanger, chorus, vibrato, tremolo, echo, reverb, high cut, a three-band equalizer, compressor, volume, phase invert and a limiter. Moving a slider switches its effect on; each slider and each effect has a reset button, and so does the whole panel. Effects fade in and out when switched so nothing clicks.
- **Presets**: Fizmo's voice presets (helium, deep, robot, alien, monster, radio, telephone, megaphone, cave, underwater, whisper, feminine, masculine) and a few of Rustle's own (Chipmunk, Demon, Walkie-talkie, Stadium, Ghost, Broadcast, Space robot) load straight into the panel, so you can tweak them. Save your own, rename and delete them; they're Fizmo preset files in Rustle's `presets` folder.
- **Output**: an Automatic mode that uses your default microphone and speakers, makes Rustle Mic, and puts things back together when a device or the sound server comes back. Or choose the microphone and speakers yourself. Separate volumes for what others hear and what you hear, a voice volume, mic on/off, push to talk, "hear myself", and live level meters for the microphone, your changed voice, what goes to others and what comes out of your speakers.
- **Files**: Rustle's own file browser (no system dialog): your home folder and the usual places, mounted drives and USB sticks, a path bar you can type into, and hidden files when you want them. Add the folder you're in to the library, or tick sound files and copy them into a library folder.
- **Keys**: a key for every sound, plus Stop all (F8), Mic on/off (F9) and push to talk. They work while Rustle has focus, and with global hotkeys on they also work while you're in a game (see below).
- **Command line**: `rustle ctl stop-all`, `rustle ctl play "air horn"`, `rustle ctl ptt-down` and friends drive the running Rustle, which is handy for stream decks and desktop shortcuts.

## Using Rustle Mic

1. Start Rustle. With Automatic on (the default) the pill at the top turns green and says **Rustle Mic ready**.
2. In **Discord**: User Settings, Voice & Video, Input Device: **Rustle Mic**. Turn off Discord's noise suppression (Krisp) and "Automatically determine input sensitivity", or set the sensitivity low: they treat the soundboard as background noise and cut it.
3. In **OBS**: add an *Audio Input Capture* source and pick **Rustle Mic**.
4. In **games** and other programs, pick **Rustle Mic** wherever they ask for a microphone. Programs that only use the default microphone can be moved over in `pavucontrol` (Recording tab) or your desktop's sound settings. You can also make Rustle Mic your default microphone: Rustle notices and records from your real microphone instead, so it never hears itself.

Rustle Mic is made with two PulseAudio modules, `module-null-sink` (*Rustle Output*, which Rustle plays into) and `module-remap-source` (*Rustle Mic*, which records it). They work the same under PipeWire through `pipewire-pulse`. Rustle loads them through `libpulse` when it starts, reuses them if they're still there from last time, and unloads them when it closes, unless you turn on *Keep Rustle Mic after Rustle closes*. If `libpulse` isn't installed, the Output page says so; install `libpulse0` (Debian, Ubuntu), `pulseaudio-libs` (Fedora) or `libpulse` (Arch).

To make the devices by hand instead (for example to keep them for good in your PipeWire or PulseAudio config):

```sh
pactl load-module module-null-sink sink_name=rustle_sink rate=48000 channels=2 sink_properties='device.description="Rustle Output"'
pactl load-module module-remap-source master=rustle_sink.monitor source_name=rustle_mic source_properties='device.description="Rustle Mic"'
```

**Windows and macOS** have no PulseAudio, so there's no Rustle Mic. Install a virtual audio cable (VB-CABLE on Windows, BlackHole on macOS), pick its input as *Or send to* on the Output page, and choose the cable as the microphone in Discord or OBS. On macOS, Rustle asks for microphone access the first time.

## Global hotkeys

- **X11**: turn on *Work in other programs too* in Settings and Rustle grabs its keys for the whole desktop (through `libX11`, loaded when needed). Settings lists the keys that worked and any another program already holds.
- **Wayland** doesn't let programs grab keys for the whole desktop. Rustle's X11 grab still works while an X11 (XWayland) window, such as many games, has focus, but not over Wayland windows. For keys that work everywhere, add custom shortcuts in your desktop's keyboard settings (GNOME, KDE and the rest all have them) that run commands like `rustle ctl stop-all`, `rustle ctl toggle-mic` or `rustle ctl play "air horn"`. Push to talk needs a key press *and* release, which desktop shortcuts can't send, so on Wayland use a toggle (`rustle ctl toggle-mic`) instead.
- **Windows**: global hotkeys work everywhere, push to talk included.
- **macOS**: keys only work while Rustle has focus for now.

## Command line

```
rustle                         open the window
rustle --check                 load the window and its script, check the audio code, and exit (works headless)
rustle --version
rustle play FILE [--to-mic]    play a file to your speakers, or into Rustle Mic, and wait for it
rustle render IN OUT.wav       apply sound edits (--speed, --tempo, --pitch, --reverse, --reverb, --volume, --start, --end)
rustle voice IN OUT.wav        run a recording through the voice changer (--preset NAME, --set pitch.semitones=5)
rustle devices                 list the sound devices Rustle sees
rustle ctl COMMAND             tell the running Rustle to: stop-all, toggle-mic, mic-on, mic-off, ptt-down, ptt-up,
                               play NAME, toggle NAME, stop NAME, preset NAME, voice-off, status
```

`rustle ctl` talks to the window over a socket in `$XDG_RUNTIME_DIR` (Linux and macOS).

## Settings

Everything is kept in `$XDG_CONFIG_HOME/rustle` (usually `~/.config/rustle`; `%APPDATA%\Rustle` on Windows and `~/Library/Application Support/Rustle` on macOS): `settings.json` holds your folders, favorites, sound edits, keys, devices, volumes and the voice panel, and `presets/` holds your saved voice presets. Settings are written to a temporary file and renamed into place. If `settings.json` can't be read, Rustle moves it aside as `settings.json.broken-<time>`, starts with defaults and tells you.

| Variable | |
|---|---|
| `RUSTLE_CONFIG_DIR` | keep settings somewhere else |
| `RUSTLE_UI_DIR` | load the UI from this folder instead of the built-in copy |
| `RUSTLE_CONTROL_SOCKET` | the socket `rustle ctl` uses |
| `FIZMO_AUDIO_BACKEND` | `null` runs without a sound card (tests, servers); `pulse` or `alsa` picks a backend |

## Building

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/bin/rustle
```

CMake fetches Fizmo, Grove and Walnut at the tags in `CMakeLists.txt`. On Debian and Ubuntu you'll need `libx11-dev libxft-dev libxrender-dev libfontconfig1-dev libfreetype-dev libssl-dev libpango1.0-dev libcairo2-dev zlib1g-dev libjpeg-dev libvulkan-dev libgl-dev`; on Fedora `libX11-devel libXft-devel libXrender-devel fontconfig-devel freetype-devel openssl-devel pango-devel cairo-devel zlib-devel libjpeg-turbo-devel vulkan-headers mesa-libGL-devel libstdc++-static`. Rustle needs no PulseAudio, PipeWire or X11 development packages: it loads `libpulse` and `libX11` at run time. `-DRUSTLE_BUILD_APP=OFF` builds just the `rustle_core` library and the tests; `ctest` runs them (with the null audio backend).

The code is in `Include/rustle` and `Source/rustle`: `library` (scanning and browsing folders), `files` (the file browser), `clip` (rendering sound edits), `voice` (the effect panel and presets), `engine` (audio streams, mixing and routing), `virtual_mic` (the PulseAudio modules), `hotkeys`, `control` (`rustle ctl`), `settings` and `session`, which ties them together. `Tools/rustle.cpp` is the program: it hands the session to the Grove UI in `UI/` and holds the command line.

## Releases

Pushing a `v*` tag builds packages for Linux (x86_64, arm64, i686), Windows (x86_64, i686, arm64) and macOS (arm64, x86_64). The tag has to match `project(rustle VERSION ...)` in `CMakeLists.txt`.
