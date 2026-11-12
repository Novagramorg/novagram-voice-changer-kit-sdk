# Novagram Voice Changer Kit

The voice changer shared by the Novagram clients: voice messages and calls, with the same presets and the same
sound as the Novagram Android app (child, woman, man, helium, hexafluoride, monster, mouse, robot, alien, echo, cave,
hoarseness, modulation, noise, and four tape speeds).

- Plain C, 16-bit mono PCM, no platform dependency and no external dependency.
- `nv_effect_*` for recordings (output may be longer or shorter than the input; `drain` returns the tail).
- `nv_call_*` for live calls: an atomic choice and an in-place 10 ms buffer path that keeps every buffer's size.
- Preset ids match Android's and are stored in settings, so they never change.

Build: SwiftPM (`swift build`, `swift run NovagramVoiceDSPTests`) or Bazel (`BUILD`, as a Telegram-iOS submodule).

License: GPL-2.0-or-later. `NovagramPitchShift.c/.h` derive from Sonic (Apache 2.0); see `CREDITS.md`.
