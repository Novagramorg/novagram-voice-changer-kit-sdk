// SPDX-License-Identifier: GPL-2.0-or-later
//
// The preset table. The fixed numbers (semitones, formant, rate and every stage parameter) are Android's VoicePreset
// table (org.fenixuz.voice.VoiceEffect.kt), so both apps sound alike; the pitch-aware targets and ranges are v2's.
#include <algorithm>
#include <cstring>

#include "nvc.h"

namespace {

nvc_preset_desc base(nvc_preset_id id) {
    nvc_preset_desc d;
    std::memset(&d, 0, sizeof(d));
    d.id = id;
    d.rate = 1;
    d.reverb_room = 0.5f;
    d.reverb_damp = 0.5f;
    d.gain = 1;
    return d;
}

/// A shift that keeps the formants where `formant` says (Android's non-null formant).
void fixedShift(nvc_preset_desc &d, float semitones, float formant) {
    d.semitones = semitones;
    d.keeps_formants = 1;
    d.formant = formant;
}

/// Pitch-aware: the shift aims at `targetHz` from the speaker's pitch, within [minSt, maxSt]. The formants move by
/// `perSemitone` of the shift, within [minF, maxF]: a speaker already near the target keeps their formants, one far
/// from it gets the full move (the max is what a 120 Hz male voice gets).
void aimAt(nvc_preset_desc &d, float targetHz, float minSt, float maxSt, float perSemitone, float minF, float maxF) {
    d.target_hz = targetHz;
    d.min_semitones = minSt;
    d.max_semitones = maxSt;
    d.formant_per_semitone = perSemitone;
    d.min_formant = minF;
    d.max_formant = maxF;
}

int clampStep(int value, int low, int high) {
    return std::min(high, std::max(low, value));
}

} // namespace

extern "C" {

const char *nvc_version(void) {
    return NVC_VERSION;
}

void nvc_params_default(nvc_params *params) {
    if (!params) {
        return;
    }
    // Graph Messenger's defaults, as on Android: +5 semitones; an echo at 70 % after 0.5 s; noise at 70 %.
    params->tone = 5;
    params->echo_delay = 5;
    params->echo_volume = 7;
    params->noise_volume = 7;
    params->retune_speed = 0.1f;
    params->pitch_aware = 1;
}

int32_t nvc_preset_describe(nvc_preset_id id, const nvc_params *paramsOrNull, nvc_preset_desc *out) {
    if (!out) {
        return NVC_ERR_ARGUMENT;
    }
    nvc_params params;
    nvc_params_default(&params);
    if (paramsOrNull) {
        params = *paramsOrNull;
    }
    nvc_preset_desc d = base(id);
    switch (id) {
    case NVC_PRESET_OFF:
        break;
    case NVC_PRESET_CHILD:
        // A boy of 8-10.
        fixedShift(d, 12, 5.5f);
        d.gain = 1.1f;
        aimAt(d, 270, 1, 16, 0.36f, 1, 5);
        break;
    case NVC_PRESET_WOMAN:
        fixedShift(d, 9, 3.5f);
        d.gain = 1.1f;
        aimAt(d, 210, -2, 12, 0.36f, 0, 3.5f);
        break;
    case NVC_PRESET_MAN:
        // A 125 Hz male voice becomes a natural 110 Hz (Android's fixed -6 st gave 88 Hz); the formants go down only
        // for a higher voice, at most 3 st.
        fixedShift(d, -6, -3);
        aimAt(d, 110, -13, 1, 0.3f, -3, 0);
        break;
    case NVC_PRESET_LITTLE_KID:
        // A 4-6 year old. Not on Android: the fixed numbers are for pitch_aware = 0.
        fixedShift(d, 15, 6);
        d.gain = 1.1f;
        aimAt(d, 330, 3, 19, 0.4f, 2, 7);
        break;
    case NVC_PRESET_YOUNG_GIRL:
        // A girl of 14-17: a little higher and brighter than Woman. Not on Android.
        fixedShift(d, 10, 4);
        d.gain = 1.1f;
        aimAt(d, 240, 0, 14, 0.375f, 0.5f, 4.5f);
        break;
    case NVC_PRESET_HELIUM:
        // As in the real gas, mostly the formants go up, the pitch only a little. Not pitch-aware: a gas does this to
        // every voice alike.
        fixedShift(d, 3, 9);
        d.gain = 0.95f;
        break;
    case NVC_PRESET_HEXAFLUORIDE:
        fixedShift(d, -3, -7);
        d.gain = 1.08f;
        break;
    case NVC_PRESET_MONSTER:
        fixedShift(d, -8, -5);
        d.drive = 2.5f;
        d.ring_hz = 30;
        d.ring_mix = 0.25f;
        d.gain = 0.54f;
        aimAt(d, 65, -14, -5, 0.625f, -7, -3);
        break;
    case NVC_PRESET_MOUSE:
        // A cartoon voice: the formants move with the pitch.
        d.semitones = 14;
        aimAt(d, 380, 8, 16, 0, 0, 0);
        break;
    case NVC_PRESET_ROBOT:
        d.robot_hz = 120;
        d.gain = 1.15f;
        break;
    case NVC_PRESET_ALIEN:
        d.semitones = 4;
        d.ring_hz = 140;
        d.ring_mix = 0.6f;
        d.gain = 1.6f;
        break;
    case NVC_PRESET_ECHO: {
        float volume = float(clampStep(params.echo_volume, 1, 10)) / 10.0f;
        float mix = volume * 0.8f;
        d.echo_ms = float(clampStep(params.echo_delay, 1, 10)) * 100.0f;
        d.echo_mix = mix;
        d.echo_feedback = 0.25f + 0.3f * volume;
        d.gain = 1.0f / (1.0f + 0.35f * mix);
        break;
    }
    case NVC_PRESET_CAVE:
        d.reverb_mix = 0.3f;
        d.reverb_room = 0.7f;
        d.reverb_damp = 0.3f;
        d.reverb_pre_delay_ms = 35;
        break;
    case NVC_PRESET_HOARSENESS:
        d.whisper = 0.5f;
        d.roughness = 0.15f;
        d.drive = 1.3f;
        break;
    case NVC_PRESET_MODULATION: {
        // A pitch the user picks; the formants go half as far (between a cartoon voice and another person's).
        int tone = clampStep(params.tone, -12, 12);
        if (tone != 0) {
            fixedShift(d, float(tone), float(tone) / 2.0f);
        }
        break;
    }
    case NVC_PRESET_NOISE:
        d.noise = 0.03f * float(clampStep(params.noise_volume, 1, 10)) / 10.0f;
        break;
    case NVC_PRESET_SPEED_0_70:
        d.rate = 0.7f;
        break;
    case NVC_PRESET_SPEED_0_85:
        d.rate = 0.85f;
        break;
    case NVC_PRESET_SPEED_1_25:
        d.rate = 1.25f;
        break;
    case NVC_PRESET_SPEED_1_50:
        d.rate = 1.5f;
        break;
    case NVC_PRESET_AUTOTUNE:
        d.autotune = 1;
        d.keeps_formants = 1;
        break;
    default:
        return NVC_ERR_ARGUMENT;
    }
    if (!params.pitch_aware) {
        d.target_hz = 0;
    }
    *out = d;
    return NVC_OK;
}

} // extern "C"
