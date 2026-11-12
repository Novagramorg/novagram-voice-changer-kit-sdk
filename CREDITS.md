# Credits

The kit has no dependencies: no package dependencies, no git submodules, nothing is fetched at build or test time.
Code adapted from other projects lives in our own source tree under our names, trimmed to what we use. Each derived
file keeps the original copyright and MIT permission notice at its top, as the MIT license requires; our changes are
GPL-2.0-or-later like the rest of the kit.

| Our file | Derived from | Project | Commit | License |
| --- | --- | --- | --- | --- |
| `Sources/NovagramVoiceChangerCore/spectral/stretcher.hpp` | `signalsmith-stretch.h` | [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) 1.4.0 | `a670068d9aeb64913331d5cc29337b19a457a7df` | MIT, Copyright (c) 2022 Geraint Luff / Signalsmith Audio Ltd. |
| `Sources/NovagramVoiceChangerCore/spectral/stft.hpp` | `stft.h` (`DynamicSTFT`) | [signalsmith-linear](https://github.com/Signalsmith-Audio/linear) 0.6.4 | `de55e6a50ffcf6f8f43f649692d94691c7025151` | MIT, Copyright (c) 2025 Signalsmith Audio |
| `Sources/NovagramVoiceChangerCore/spectral/fft.hpp` | `fft.h` (`RealFFT`, half-bin-shifted packing only) | [signalsmith-linear](https://github.com/Signalsmith-Audio/linear) 0.6.4 | `de55e6a50ffcf6f8f43f649692d94691c7025151` | MIT, Copyright (c) 2025 Signalsmith Audio |

What we changed:

- `stretcher.hpp`: namespace `nvc::spectral`, class `Stretcher`; a seeded random engine (the same input always gives the
  same output); removed seeking (`seek`, `outputSeek`, `exact`), custom frequency maps, split computation, the preset
  helpers and the profiling hooks. The phase vocoder, pitch and formant code is unchanged.
- `stft.hpp`: only the half-bin-shifted spectrum, Kaiser windows and round-trip normalisation; no split computation, time
  hooks, alternative spellings or the confined-Gaussian window.
- `fft.hpp`: our own radix-2 complex FFT (power-of-two sizes) under the real-FFT packing from signalsmith-linear; no
  mixed-radix sizes, SIMD or platform back-ends.

Checked against the originals: on a 150 Hz harmonic signal, +5 semitones with formant compensation, 2048-sample
blocks, `Stretcher` and the original `SignalsmithStretch` differ by at most 2.4e-4 (the peak is 0.64), which is float
rounding in the different FFT.

For the record, the archives the code was read from (`codeload.github.com/.../tar.gz/<commit>`): Signalsmith Stretch
sha256 `948b8382bb13f316d13536cdfe06111789a538e8fee7f7a531ee2aec94de1747`, signalsmith-linear sha256
`cc39693d5c8bc96451b5503819bb82b2d88c1df450183c55595e5d9d4b517012`.

Not derived from other code, but based on published work or our own apps:

- The pitch tracker (`pitch_tracker.cpp`) implements YIN from A. de Cheveigne and H. Kawahara, "YIN, a fundamental
  frequency estimator for speech and music", JASA 111(4), 2002. Our own code.
- The preset table (`presets.cpp`) uses the numbers of the Novagram Android voice changer (ours), whose 15 presets follow
  Graph Messenger's list.
