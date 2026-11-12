// SPDX-License-Identifier: GPL-2.0-or-later
//
// Novagram Voice Changer Kit: the plain C API over the C++ engine, for Telegram's ObjC++/WebRTC code, Swift and
// Android JNI alike. Mono 16-bit PCM in, mono 16-bit PCM out.
//
// Threading: an engine is used by one audio thread (the process / drain calls). In call mode nvc_engine_set_preset
// and the nvc_engine_* getters may be called from any other thread at any time; they never block the audio thread.
// In message mode everything happens on one thread.
#ifndef NOVAGRAM_VOICE_CHANGER_NVC_H
#define NOVAGRAM_VOICE_CHANGER_NVC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVC_VERSION "2.0.0"

/// Return codes. Functions that return a sample count return it as a non-negative number, or one of these.
enum {
    NVC_OK = 0,
    NVC_ERR_ARGUMENT = -1, ///< A null engine or buffer, a count out of range, or an unknown preset.
    NVC_ERR_MODE = -2,     ///< A message call on a call engine, or the other way round.
    NVC_ERR_CAPACITY = -3, ///< The output buffer is smaller than nvc_message_output_bound().
    NVC_ERR_STATE = -4,    ///< Message mode: the preset can only change before the first chunk.
};

typedef int32_t nvc_mode;
enum {
    /// Voice messages: output size varies per call, nvc_message_drain_i16() returns the tail, tape speed is allowed,
    /// latency does not matter (the engine looks one second ahead to learn the speaker's pitch first).
    NVC_MODE_MESSAGE = 0,
    /// Calls: fixed blocks (10 ms) changed in place, constant latency, no allocation or locks on the audio thread.
    NVC_MODE_CALL = 1,
};

/// Preset ids, the same numbers as the Novagram Android voice changer (stored in preferences: never renumber).
typedef int32_t nvc_preset_id;
enum {
    NVC_PRESET_OFF = 0,
    NVC_PRESET_CHILD = 1,
    NVC_PRESET_WOMAN = 2,
    NVC_PRESET_MAN = 3,
    NVC_PRESET_HELIUM = 4,
    NVC_PRESET_HEXAFLUORIDE = 5,
    NVC_PRESET_MONSTER = 6,
    NVC_PRESET_MOUSE = 7,
    NVC_PRESET_ROBOT = 8,
    NVC_PRESET_ALIEN = 9,
    NVC_PRESET_ECHO = 10,
    NVC_PRESET_CAVE = 12,
    NVC_PRESET_HOARSENESS = 13,
    NVC_PRESET_MODULATION = 14,
    NVC_PRESET_NOISE = 15,
    NVC_PRESET_SPEED_0_70 = 16, ///< Tape speed 0.7 (slower, thicker); message mode only, off in calls.
    NVC_PRESET_SPEED_0_85 = 17,
    NVC_PRESET_SPEED_1_25 = 18,
    NVC_PRESET_SPEED_1_50 = 19, ///< Tape speed 1.5 (faster, thinner).
    NVC_PRESET_LITTLE_KID = 20, ///< v2 only: a 4-6 year old (about 330 Hz, formants up to +7 st).
    NVC_PRESET_YOUNG_GIRL = 21, ///< v2 only: a 14-17 year old girl (about 240 Hz, formants up to +4.5 st).
    NVC_PRESET_AUTOTUNE = 22,   ///< v2 only: the pitch snapped to the nearest semitone.
};

/// The settings a user can change, in Android's steps.
typedef struct nvc_params {
    int32_t tone;         ///< Modulation: semitones, -12..12 (default 5).
    int32_t echo_delay;   ///< Echo: delay in tenths of a second, 1..10 (default 5).
    int32_t echo_volume;  ///< Echo: volume in tenths, 1..10 (default 7).
    int32_t noise_volume; ///< Noise: volume in tenths, 1..10 (default 7).
    float retune_speed;   ///< Auto-tune: 0 = instant snap (the robotic effect) .. 1 = slow, natural (default 0.1).
    int32_t pitch_aware;  ///< 1 (default): man/woman/child/mouse/monster aim at a pitch from the speaker's own; 0: Android's fixed shifts.
} nvc_params;

/// Everything a preset does, with Android's numbers for the stages after the pitch engine, so that a stage chain can be
/// built from the same table. Filled by nvc_preset_describe().
typedef struct nvc_preset_desc {
    nvc_preset_id id;
    // Pitch engine (this SDK).
    float semitones;            ///< Android's fixed shift; for pitch-aware presets also the shift until the speaker's pitch is known.
    int32_t keeps_formants;     ///< 0: the formants move with the pitch (cartoon voice); 1: they move by `formant` only.
    float formant;              ///< Semitones the formants move, apart from the pitch (when keeps_formants).
    float rate;                 ///< Tape speed, 1 = none.
    float target_hz;            ///< Pitch-aware: the pitch the voice is moved to; 0 = not pitch-aware.
    float min_semitones;        ///< Pitch-aware: the shift is clamped to [min_semitones, max_semitones].
    float max_semitones;
    float formant_per_semitone; ///< Pitch-aware: formant = shift * formant_per_semitone, clamped to [min_formant, max_formant]
                                ///< (about 0.36: a woman's voice is ~9.7 st above a man's, her formants ~3.5 st).
    float min_formant;
    float max_formant;
    int32_t autotune;           ///< 1: snap the pitch to the nearest semitone.
    // Stages after the pitch engine (ported separately), Android's VoicePreset fields.
    float robot_hz, ring_hz, ring_mix;
    float echo_ms, echo_feedback, echo_mix;
    float reverb_mix, reverb_room, reverb_damp, reverb_pre_delay_ms;
    float drive, whisper, roughness, noise;
    float gain; ///< Applied by the engine after the stage chain, before its soft limiter.
} nvc_preset_desc;

/// The seam for the stages after the pitch engine (robot/whisper, ring, drive, echo, reverb, noise). The engine runs
/// `process` on every block of pitch-shifted voice, in place, mono float in -1..1 at the engine's sample rate; then it
/// applies the preset's gain and its soft limiter. The chain must not apply the gain or a limiter itself.
/// Each nvc_engine_set_preset() hands over one chain; the engine calls `release` exactly once, on a non-audio thread
/// (inside a later nvc_engine_set_preset or nvc_engine_destroy), when the audio thread can no longer touch it.
typedef struct nvc_stage_chain {
    void *context;
    /// Audio thread: no allocation, locks or blocking. Required.
    void (*process)(void *context, float *samples, int32_t count);
    /// Optional, may be null.
    void (*release)(void *context);
    /// Samples the chain delays the voice by (an STFT stage's lag); added to the reported latency and drained.
    int32_t latency_samples;
    /// Samples of ring-out after the voice ends (echo, reverb); message drain fades them out linearly.
    int32_t tail_samples;
} nvc_stage_chain;

typedef struct nvc_engine nvc_engine;

const char *nvc_version(void);

void nvc_params_default(nvc_params *params);

/// Fills `out` for a preset id with `params` applied (null = defaults). Returns NVC_OK or NVC_ERR_ARGUMENT.
int32_t nvc_preset_describe(nvc_preset_id id, const nvc_params *params, nvc_preset_desc *out);

/// sample_rate: 8000..192000. max_block (call mode): the largest count nvc_call_process_i16 will get (e.g. 480 for
/// 10 ms at 48 kHz); ignored in message mode. Returns null on bad arguments. The new engine is Off.
nvc_engine *nvc_engine_create(nvc_mode mode, int32_t sample_rate, int32_t max_block);
void nvc_engine_destroy(nvc_engine *engine);

/// params and chain may be null. Call mode: any thread, switches without a click (20 ms crossfade); a tape-speed
/// preset counts as Off. Message mode: only before the first chunk (else NVC_ERR_STATE).
int32_t nvc_engine_set_preset(nvc_engine *engine, nvc_preset_id id, const nvc_params *params, const nvc_stage_chain *chain);

/// Call mode: the constant delay of the changed voice in samples (pitch engine + chain), 0 while Off.
/// Message mode: 0 (the output is aligned with the input).
int32_t nvc_engine_latency_samples(const nvc_engine *engine);
/// The speaker's pitch in Hz (a slow median of voiced frames), 0 until about 0.2 s of voice was heard.
float nvc_engine_speaker_pitch_hz(const nvc_engine *engine);
/// The pitch shift in semitones currently applied.
float nvc_engine_shift_semitones(const nvc_engine *engine);

/// Call mode: changes `count` samples in place (count <= max_block). Returns count or an error.
int32_t nvc_call_process_i16(nvc_engine *engine, int16_t *samples, int32_t count);

/// Message mode: the most samples nvc_message_process_i16 can return for `input_count` input samples.
int32_t nvc_message_output_bound(const nvc_engine *engine, int32_t input_count);
/// Message mode: processes a chunk and writes what is ready. Returns the number written or an error.
int32_t nvc_message_process_i16(nvc_engine *engine, const int16_t *input, int32_t input_count, int16_t *output, int32_t output_capacity);
/// Message mode: after the last chunk, returns the rest (lookahead, pitch engine, stage tail), up to
/// `output_capacity` per call. Call until it returns 0.
int32_t nvc_message_drain_i16(nvc_engine *engine, int16_t *output, int32_t output_capacity);

#ifdef __cplusplus
}
#endif

#endif
