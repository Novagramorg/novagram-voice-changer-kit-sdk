# Novagram Voice Changer Kit

The voice changer shared by the Novagram iOS and macOS clients and, through its C API, the Android client: voice
presets for voice messages and calls (man, woman, boy, little kid, young girl, mouse, monster, helium, robot, echo,
cave, auto-tune and more).

v2 engine:

- **Pitch-aware presets.** A YIN pitch tracker learns the speaker's pitch (a slow median of voiced frames), and Man,
  Woman, Child (a boy), Little kid, Young girl, Mouse and Monster aim at a pitch (about 110, 210, 270, 330, 240, 380
  and 65 Hz) instead of a fixed number of semitones, so a low and a high voice come out alike: a 125 Hz man through
  Man stays a natural 110 Hz (Android's fixed -6 st gave 88 Hz). The shift is clamped per preset, held by a deadband and glided,
  so it follows the speaker and never the syllable. Until the speaker is known the preset uses Android's fixed shift.
- **Pitch and formants** by a phase vocoder adapted from Signalsmith Stretch: man/woman/child move the formants by a
  share of the shift (another person), cartoon voices (Mouse) move them with the pitch.
- **Auto-tune**: the pitch snapped to the nearest semitone, with a retune speed.
- **Message mode** looks one second ahead, so the speaker's pitch is known from the first word; output is aligned with
  the input. **Call mode** changes 10 ms blocks in place at a constant 50 ms latency, switches presets without a click
  and never allocates or locks on the audio thread.
- No dependencies. See [CREDITS.md](CREDITS.md) for the code adapted from other projects.

The stages after the pitch engine (robot/whisper, ring modulation, drive, echo, reverb, noise) are being ported
separately and plug into the engine through the stage-chain seam below. Without a chain those presets play only their
pitch part (Alien, Monster) or pass the voice through (Robot, Echo, Cave, Hoarseness, Noise; gain aside).

## License

GPL-2.0-or-later (`LICENSE`). Every source file starts with an SPDX header. The three files in
`Sources/NovagramVoiceChangerCore/spectral/` are derived from MIT code and keep its notice
(`GPL-2.0-or-later AND MIT`); see [CREDITS.md](CREDITS.md).

## Products

| Product / Bazel target     | Contents |
| -------------------------- | -------- |
| `NovagramVoiceChangerCore` | The C++17 engine and the plain C API, `include/nvc.h` (prefix `nvc_`). For ObjC++/WebRTC and Android JNI. |
| `NovagramVoiceChanger`     | Swift: `VoiceChangerPreset` (raw values = Android ids), `VoiceChangerParams`, `VoiceChanger`. |
| `nvc-render` (SwiftPM only) | Renders every preset of a 16-bit WAV for listening: `swift run -c release nvc-render [input.wav] [folder]` writes `message-<preset>.wav` and `call-<preset>.wav` to `samples/` (gitignored) and prints each one's pitch in and out. Without an input it synthesises a voice. |

Platforms: iOS 15, macOS 10.13. SwiftPM: swift-tools 6.0, Swift 6 language mode. Bazel: `BUILD` has an `objc_library`
for the core (compiled `-O2` even in debug) and a `swift_library` for the wrapper, like the call recording kit.

## Presets

Ids are Android's and are stored in preferences, so they never change.

| Id | Preset | Pitch engine | Stage chain (Android numbers) |
| -- | ------ | ------------ | ----------------------------- |
| 0 | Off | bit-identical passthrough | |
| 1 | Child ("bola", a boy of 8-10) | aims at 270 Hz (+1..+16 st), formants 0.36 of the shift (+1..+5) | gain 1.1 |
| 2 | Woman | aims at 210 Hz (-2..+12 st), formants 0.36 of the shift (0..+3.5) | gain 1.1 |
| 3 | Man | aims at 110 Hz (-13..+1 st), formants 0.3 of the shift (-3..0): down only for a higher voice | |
| 4 | Helium | +3 st, formants +9 | gain 0.95 |
| 5 | Hexafluoride | -3 st, formants -7 | gain 1.08 |
| 6 | Monster | aims at 65 Hz (-14..-5 st), formants (-7..-3) | drive 2.5, ring 30 Hz, gain 0.54 |
| 7 | Mouse | aims at 380 Hz (+8..+16 st), cartoon | |
| 8 | Robot | | robot 120 Hz, gain 1.15 |
| 9 | Alien | +4 st, cartoon | ring 140 Hz, gain 1.6 |
| 10 | Echo | | echo from the params |
| 12 | Cave | | reverb |
| 13 | Hoarseness | | whisper, roughness, drive |
| 14 | Modulation | the user's semitones, formants half as far | |
| 15 | Noise | | noise from the params |
| 16-19 | Tape speed 0.7 / 0.85 / 1.25 / 1.5 | resampled: length / rate (messages only; Off in calls) | |
| 20 | Little kid ("yosh bola", 4-6, v2) | aims at 330 Hz (+3..+19 st), formants 0.4 of the shift (+2..+7) | gain 1.1 |
| 21 | Young girl ("yosh qiz", 14-17, v2) | aims at 240 Hz (0..+14 st), formants 0.375 of the shift (+0.5..+4.5) | gain 1.1 |
| 22 | Auto-tune (v2) | chromatic snap, retune speed | |

The formant share is about 0.36 because a woman's voice is ~9.7 st above a man's while her formants are ~3.5 st higher;
so a speaker already near the target keeps their own formants, and one far from it gets the full move.
`nvc_params.pitch_aware = 0` gives fixed shifts: Android's for Child +12, Woman +9, Man -6, Mouse +14, Monster -8, and
Little kid +15, Young girl +10.
`nvc_preset_describe()` returns all of a preset's numbers, including the stage parameters, so a stage chain can be
built from the same table.

## C API

```c
#include "nvc.h"

// Voice message: chunks of any size in, variable size out, then the tail.
nvc_engine *engine = nvc_engine_create(NVC_MODE_MESSAGE, 48000, 0);
nvc_engine_set_preset(engine, NVC_PRESET_WOMAN, NULL /* default params */, NULL /* no stage chain */);
int16_t out[nvc_message_output_bound(engine, count)];
int32_t written = nvc_message_process_i16(engine, input, count, out, capacity);
while ((written = nvc_message_drain_i16(engine, out, capacity)) > 0) { /* append */ }
nvc_engine_destroy(engine);

// Call: 10 ms blocks in place on the audio thread; nvc_engine_set_preset from any other thread at any time.
nvc_engine *call = nvc_engine_create(NVC_MODE_CALL, 48000, 480);
nvc_call_process_i16(call, samples, 480);
int32_t latency = nvc_engine_latency_samples(call); // 2400 (50 ms) while a preset is on, 0 when Off
```

Other calls: `nvc_version`, `nvc_params_default`, `nvc_preset_describe`, `nvc_engine_speaker_pitch_hz`,
`nvc_engine_shift_semitones`. Errors are negative: `NVC_ERR_ARGUMENT`, `NVC_ERR_MODE`, `NVC_ERR_CAPACITY`,
`NVC_ERR_STATE` (a message's preset can't change after its first chunk).

### Stage-chain seam

```c
typedef struct nvc_stage_chain {
    void *context;
    void (*process)(void *context, float *samples, int32_t count); // audio thread, in place
    void (*release)(void *context);                                // optional
    int32_t latency_samples;                                       // reported and drained
    int32_t tail_samples;                                          // message drain rings out and fades
} nvc_stage_chain;

nvc_engine_set_preset(engine, id, &params, &chain);
```

Order: pitch engine (or tape speed) → `chain.process` (mono float, -1..1, in place, at the engine's rate) → preset gain
→ soft limiter → 16 bits. The chain must not apply the gain or a limiter. Each `set_preset` hands over one chain; the
engine calls `release` exactly once, never on the audio thread. In a call the old and new chains run side by side
during the 20 ms crossfade.

## Swift

```swift
import NovagramVoiceChanger

let changer = VoiceChanger(mode: .message, sampleRate: 48000)!
changer.setPreset(.woman, params: VoiceChangerParams(pitchAware: true))
var message = changer.process(chunk)   // per recorded chunk
message += changer.drain()             // after the last one

let call = VoiceChanger(mode: .call, sampleRate: 48000)!   // 10 ms blocks
call.setPreset(.autoTune, params: VoiceChangerParams(retuneSpeed: 0))
call.processCall(blockPointer)         // in place
```

## Threading

- Call mode: one audio thread calls `nvc_call_process_i16`. `nvc_engine_set_preset` and the getters are safe from any
  thread; a preset travels to the audio thread through a lock-free triple buffer. All memory is allocated in
  `nvc_engine_create`; the audio thread never allocates, locks or frees.
- Message mode: one thread for everything (the drain allocates its tail buffer).

## Tests

`swift test` (13 tests, also `swift test -c release`; the benchmark asserts only in release). Numbers from the last run:

| Test | Result |
| ---- | ------ |
| Off (and Modulation 0) bit-identical, message and call | passes |
| Woman, 120 Hz harmonic sawtooth | 210.1 Hz out (message), 210.5 Hz (call, 48 kHz) |
| Woman, 220 Hz | 210.0 Hz (message from 0.2 s), 210.1 Hz (call, 16 kHz); Android's fixed +9 would give 370.1 Hz |
| Man / Woman / Child / Little kid / Young girl, from 120 Hz | 110.0 / 210.1 / 270.2 / 330.2 / 240.2 Hz |
| the same from 220 Hz | 110.0 / 210.0 / 270.0 / 330.0 / 240.0 Hz |
| Man from 125 Hz; Monster from 120 Hz | 110.0 Hz; 65.0 Hz |
| Auto-tune 452 Hz / 229 Hz | 439.7 / 233.1 Hz |
| Speaker pitch, 150 Hz | 150.03 Hz; white noise stays unvoiced |
| Tape 1.5 / 0.7 on 96000 samples | 64001 / 137145 samples |
| Call blocks | every block returns its own size; latency reported 50.0 ms, measured 48.8 ms (48 kHz) and 49.3 ms (16 kHz) |
| Preset switches every 0.3 s mid-call | largest sample step 501 for a 10000-peak sine (no clicks) |
| Full-scale input | no sample reaches full scale |
| Call mode 48 kHz realtime factor | 0.013 (release), 0.18 (debug, -O0) on Apple Silicon |

## Files

- `Sources/NovagramVoiceChangerCore/include/nvc.h`: the C API.
- `engine.hpp/.cpp`: modes, the preset crossfades, pitch-aware and auto-tune control.
- `pitch_tracker.hpp/.cpp`: YIN on a 1 kHz low-passed, ~8 kHz decimated signal, 10 ms frames, median smoothing.
- `presets.cpp`: the preset table. `tape_resampler.hpp`: tape speed. `dsp.hpp`: filters, soft limiter, triple buffer.
- `spectral/`: FFT, STFT and the Stretcher (adapted, see CREDITS.md).
- `Sources/NovagramVoiceChanger/`: the Swift layer. `Sources/nvc-render/`: the listening tool.
