// SPDX-License-Identifier: GPL-2.0-or-later
#include <new>

#include "engine.hpp"
#include "nvc.h"

struct nvc_engine : nvc::Engine {
    using nvc::Engine::Engine;
};

extern "C" {

nvc_engine *nvc_engine_create(nvc_mode mode, int32_t sample_rate, int32_t max_block) {
    if (sample_rate < 8000 || sample_rate > 192000) {
        return nullptr;
    }
    if (mode == NVC_MODE_CALL) {
        if (max_block < 1 || max_block > sample_rate) {
            return nullptr;
        }
    } else if (mode != NVC_MODE_MESSAGE) {
        return nullptr;
    }
    try {
        return new nvc_engine(mode, sample_rate, max_block);
    } catch (...) {
        return nullptr;
    }
}

void nvc_engine_destroy(nvc_engine *engine) {
    delete engine;
}

int32_t nvc_engine_set_preset(nvc_engine *engine, nvc_preset_id id, const nvc_params *params, const nvc_stage_chain *chain) {
    if (!engine) {
        return NVC_ERR_ARGUMENT;
    }
    try {
        return engine->setPreset(id, params, chain);
    } catch (...) {
        return NVC_ERR_ARGUMENT;
    }
}

int32_t nvc_engine_latency_samples(const nvc_engine *engine) {
    return engine ? engine->latencySamples() : 0;
}

float nvc_engine_speaker_pitch_hz(const nvc_engine *engine) {
    return engine ? engine->speakerPitchHz() : 0;
}

float nvc_engine_shift_semitones(const nvc_engine *engine) {
    return engine ? engine->shiftSemitones() : 0;
}

int32_t nvc_call_process_i16(nvc_engine *engine, int16_t *samples, int32_t count) {
    return engine ? engine->processCall(samples, count) : NVC_ERR_ARGUMENT;
}

int32_t nvc_message_output_bound(const nvc_engine *engine, int32_t input_count) {
    if (!engine) {
        return NVC_ERR_ARGUMENT;
    }
    return engine->mode() == NVC_MODE_MESSAGE ? engine->messageOutputBound(input_count) : NVC_ERR_MODE;
}

int32_t nvc_message_process_i16(nvc_engine *engine, const int16_t *input, int32_t input_count, int16_t *output, int32_t output_capacity) {
    return engine ? engine->processMessage(input, input_count, output, output_capacity) : NVC_ERR_ARGUMENT;
}

int32_t nvc_message_drain_i16(nvc_engine *engine, int16_t *output, int32_t output_capacity) {
    if (!engine) {
        return NVC_ERR_ARGUMENT;
    }
    try {
        return engine->drainMessage(output, output_capacity);
    } catch (...) {
        return NVC_ERR_ARGUMENT;
    }
}

} // extern "C"
