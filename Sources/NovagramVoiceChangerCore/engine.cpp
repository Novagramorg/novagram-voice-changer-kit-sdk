// SPDX-License-Identifier: GPL-2.0-or-later
#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "spectral/stretcher.hpp"

namespace nvc {

namespace {

constexpr float kDeadband = 0.3f;         // semitones the target must move before a settled shift follows it
constexpr float kGlideSlow = 0.6f;        // seconds, once settled
constexpr float kGlideFast = 0.12f;       // seconds, while learning a new speaker and right after a preset switch
constexpr float kAcquireSeconds = 1.0f;
constexpr float kSwitchSeconds = 0.15f;
constexpr float kFadeSeconds = 0.02f;
constexpr float kLookaheadSeconds = 1.0f;
constexpr float kLowestRate = 0.7f;       // the slowest tape speed, for output bounds

float clampf(float value, float low, float high) {
    return std::min(high, std::max(low, value));
}

} // namespace

/// The pitch and formant engine: our Stretcher (adapted from Signalsmith Stretch) with equal input and output sizes.
class PitchShifter {
public:
    void configure(int sampleRate, int block, int interval) {
        sampleRate_ = sampleRate;
        stretcher_.configure(1, block, interval);
    }
    int latency() const { return stretcher_.inputLatency() + stretcher_.outputLatency(); }
    int inputLatency() const { return stretcher_.inputLatency(); }
    void reset() { stretcher_.reset(); }
    void set(float semitones, bool keepFormants, float formantSemitones, float baseHz) {
        stretcher_.setTransposeSemitones(semitones);
        if (keepFormants) {
            stretcher_.setFormantSemitones(formantSemitones, true);
        } else {
            stretcher_.setFormantFactor(1, false); // the formants move with the pitch: a cartoon voice
        }
        stretcher_.setFormantBase(baseHz > 0 ? baseHz / float(sampleRate_) : 0);
    }
    void process(const float *input, float *output, int count) {
        const float *inputs[1] = {input};
        float *outputs[1] = {output};
        stretcher_.process(inputs, count, outputs, count);
    }

private:
    spectral::Stretcher<float> stretcher_;
    int sampleRate_ = 48000;
};

Engine::Engine(nvc_mode mode, int sampleRate, int maxBlock)
    : mode_(mode), sampleRate_(sampleRate), maxBlock_(maxBlock), shifter_(new PitchShifter()) {
    tracker_.configure(sampleRate);
    fadeLength_ = std::max(1, int(float(sampleRate) * kFadeSeconds));
    nvc_preset_describe(NVC_PRESET_OFF, nullptr, &current_.preset);
    nvc_params_default(&current_.params);
    if (mode == NVC_MODE_CALL) {
        // 50 ms blocks every 10 ms: 50 ms latency, and one block of work per 10 ms call (no spikes).
        shifter_->configure(sampleRate, sampleRate / 20, std::max(1, sampleRate / 100));
        chunk_ = maxBlock;
        input_.assign(size_t(maxBlock), 0.0f);
        wet_.assign(size_t(maxBlock), 0.0f);
        wetOld_.assign(size_t(maxBlock), 0.0f);
    } else {
        // Latency doesn't matter for messages, so longer blocks for a cleaner low voice; 20 ms steps keep auto-tune quick.
        int interval = std::max(1, sampleRate / 50);
        shifter_->configure(sampleRate, sampleRate / 10, interval);
        chunk_ = interval;
        input_.assign(size_t(chunk_), 0.0f);
        wet_.assign(size_t(std::ceil(chunk_ / kLowestRate)) + 16, 0.0f);
        zeros_.assign(size_t(chunk_), 0.0f);
        fifo_.assign(size_t(float(sampleRate) * kLookaheadSeconds) + size_t(chunk_) + 1, 0.0f);
    }
}

Engine::~Engine() {
    releaseRetiredChains(true);
}

// MARK: - Control side

int32_t Engine::setPreset(nvc_preset_id id, const nvc_params *params, const nvc_stage_chain *chain) {
    EngineConfig config;
    std::memset(&config, 0, sizeof(config));
    if (nvc_preset_describe(id, params, &config.preset) != NVC_OK || (chain && !chain->process)) {
        return NVC_ERR_ARGUMENT;
    }
    if (params) {
        config.params = *params;
    } else {
        nvc_params_default(&config.params);
    }
    if (mode_ == NVC_MODE_CALL && config.preset.rate != 1.0f) {
        // A tape speed can't change a live voice; as on Android, a call treats it as Off.
        nvc_preset_describe(NVC_PRESET_OFF, &config.params, &config.preset);
    }
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (mode_ == NVC_MODE_MESSAGE && messageStarted_) {
        return NVC_ERR_STATE;
    }
    config.generation = nextGeneration_++;
    if (chain) {
        config.chain = *chain;
        config.hasChain = true;
        ownedChains_.push_back({config.generation, *chain});
    }
    if (mode_ == NVC_MODE_MESSAGE) {
        current_ = config;
        oldestInUse_.store(config.generation, std::memory_order_release);
    } else {
        mailbox_.back() = config;
        mailbox_.publish();
    }
    releaseRetiredChains(false);
    return NVC_OK;
}

void Engine::releaseRetiredChains(bool all) {
    uint64_t oldest = all ? std::numeric_limits<uint64_t>::max() : oldestInUse_.load(std::memory_order_acquire);
    for (auto it = ownedChains_.begin(); it != ownedChains_.end();) {
        if (it->first < oldest) {
            if (it->second.release) {
                it->second.release(it->second.context);
            }
            it = ownedChains_.erase(it);
        } else {
            ++it;
        }
    }
}

// MARK: - Shared

bool Engine::usesShifter(const EngineConfig &config) const {
    const nvc_preset_desc &p = config.preset;
    return p.rate == 1.0f && (p.semitones != 0 || p.keeps_formants || p.target_hz > 0 || p.autotune);
}

bool Engine::isPassthrough(const EngineConfig &config) const {
    const nvc_preset_desc &p = config.preset;
    return p.id == NVC_PRESET_OFF || (p.rate == 1.0f && !usesShifter(config) && !config.hasChain && p.gain == 1.0f);
}

bool Engine::needsLookahead(const EngineConfig &config) const {
    return usesShifter(config) && (config.preset.target_hz > 0 || config.preset.autotune);
}

int32_t Engine::chainLatency(const EngineConfig &config) const {
    return config.hasChain ? std::max(0, config.chain.latency_samples) : 0;
}

void Engine::resetPitchControl() {
    appliedShift_ = heldShift_ = 0;
    shiftValid_ = speakerKnown_ = false;
    fastGlideLeft_ = 0;
    autotuneNote_ = -1;
}

void Engine::publishTrackerStats() {
    speakerPitch_.store(tracker_.speakerF0(), std::memory_order_relaxed);
}

void Engine::updatePitch(const EngineConfig &config, int count, int64_t alignSample) {
    const nvc_preset_desc &p = config.preset;
    float dt = float(count) / float(sampleRate_);
    float speaker = tracker_.speakerF0();
    if (fastGlideLeft_ > 0) {
        fastGlideLeft_ -= dt;
    }

    if (p.autotune) {
        // Chromatic auto-tune: the correction to the nearest semitone, reached at the retune speed. A note is kept
        // until the voice is 0.6 semitone away from it, so a pitch between two notes doesn't flip between them.
        float f0 = alignSample >= 0 ? tracker_.smoothedF0At(alignSample) : tracker_.smoothedF0();
        if (f0 > 0) {
            float note = 69.0f + 12.0f * std::log2(f0 / 440.0f);
            if (autotuneNote_ < 0 || std::fabs(note - autotuneNote_) > 0.6f) {
                autotuneNote_ = std::round(note);
            }
            float tau = clampf(config.params.retune_speed, 0, 1) * 0.2f;
            appliedShift_ += (autotuneNote_ - note - appliedShift_) * smoothing(dt, tau);
        } else {
            appliedShift_ += (0 - appliedShift_) * smoothing(dt, 0.05f);
        }
        shiftValid_ = true;
        shifter_->set(appliedShift_, true, 0, speaker);
        shift_.store(appliedShift_, std::memory_order_relaxed);
        return;
    }

    // Pitch-aware presets aim at a pitch from the speaker's own (a slow median), clamped to the preset's range; until
    // the speaker is known they use Android's fixed shift.
    bool aware = p.target_hz > 0 && speaker > 0;
    float target = aware ? clampf(12.0f * std::log2(p.target_hz / speaker), p.min_semitones, p.max_semitones) : p.semitones;
    if (!shiftValid_) {
        appliedShift_ = heldShift_ = target;
        shiftValid_ = true;
        speakerKnown_ = aware;
    }
    if (aware && !speakerKnown_) {
        speakerKnown_ = true;
        fastGlideLeft_ = std::max(fastGlideLeft_, kAcquireSeconds);
    }
    // A deadband and a slow glide: the shift follows the speaker, never the syllable.
    if (fastGlideLeft_ > 0 || std::fabs(target - heldShift_) > kDeadband) {
        heldShift_ = target;
    }
    appliedShift_ += (heldShift_ - appliedShift_) * smoothing(dt, fastGlideLeft_ > 0 ? kGlideFast : kGlideSlow);
    float formant = p.formant;
    if (aware && p.keeps_formants) {
        formant = clampf(p.formant_per_semitone * appliedShift_, p.min_formant, p.max_formant);
    }
    shifter_->set(appliedShift_, p.keeps_formants != 0, formant, speaker);
    shift_.store(appliedShift_, std::memory_order_relaxed);
}

void Engine::finishWet(const EngineConfig &config, float *samples, int count) {
    if (config.hasChain && count > 0) {
        config.chain.process(config.chain.context, samples, count);
    }
    float gain = config.preset.gain;
    if (gain != 1.0f) {
        for (int i = 0; i < count; ++i) {
            samples[i] *= gain;
        }
    }
}

// MARK: - Call mode

void Engine::publishInUse() {
    uint64_t oldest = current_.generation;
    if (state_ == State::chainFade) {
        oldest = std::min(oldest, previous_.generation);
    }
    if (state_ == State::fadeOut) {
        oldest = std::min(oldest, pending_.generation);
    }
    oldestInUse_.store(oldest, std::memory_order_release);
}

void Engine::pickUpConfig() {
    if (!mailbox_.update()) {
        return;
    }
    const EngineConfig &next = mailbox_.front();
    bool nextOff = next.preset.id == NVC_PRESET_OFF;
    if (state_ == State::off) {
        current_ = next;
        if (!nextOff) {
            shifter_->reset();
            resetPitchControl();
            // The changed voice arrives one latency later: until then the voice goes on as it was (or, at the start of
            // a call, silence, so the real voice is never heard), then a crossfade.
            primeSilently_ = !hasProcessed_;
            state_ = State::priming;
            stateLeft_ = shifter_->latency() + chainLatency(current_);
            latency_.store(shifter_->latency() + chainLatency(current_), std::memory_order_relaxed);
        }
    } else if (nextOff) {
        pending_ = next;
        state_ = State::fadeOut;
        stateLeft_ = fadeLength_;
    } else {
        previous_ = current_;
        current_ = next;
        state_ = State::chainFade;
        stateLeft_ = fadeLength_;
        fastGlideLeft_ = std::max(fastGlideLeft_, kSwitchSeconds);
        autotuneNote_ = -1;
        latency_.store(shifter_->latency() + chainLatency(current_), std::memory_order_relaxed);
    }
    publishInUse();
}

int32_t Engine::processCall(int16_t *samples, int32_t count) {
    if (mode_ != NVC_MODE_CALL) {
        return NVC_ERR_MODE;
    }
    if (!samples || count < 0 || count > maxBlock_) {
        return NVC_ERR_ARGUMENT;
    }
    if (count == 0) {
        return 0;
    }
    float *in = input_.data();
    for (int i = 0; i < count; ++i) {
        in[i] = float(samples[i]) * (1.0f / 32768.0f);
    }
    // The tracker runs even while Off, so a preset chosen mid-call already knows the speaker.
    tracker_.push(in, count);
    publishTrackerStats();
    if (state_ == State::off || state_ == State::wet) {
        pickUpConfig();
    }
    if (state_ == State::off) {
        hasProcessed_ = true;
        return count; // untouched: Off is bit-identical
    }

    updatePitch(current_, count, -1);
    float *wet = wet_.data();
    shifter_->process(in, wet, count);
    int done = fadeLength_ - stateLeft_;
    if (state_ == State::chainFade) {
        float *old = wetOld_.data();
        std::memcpy(old, wet, sizeof(float) * size_t(count));
        finishWet(previous_, old, count);
        finishWet(current_, wet, count);
        for (int i = 0; i < count; ++i) {
            float g = std::min(1.0f, float(done + i + 1) / float(fadeLength_));
            wet[i] = old[i] + (wet[i] - old[i]) * g;
        }
    } else {
        finishWet(current_, wet, count);
    }
    for (int i = 0; i < count; ++i) {
        wet[i] = softLimit(wet[i]);
    }
    hasProcessed_ = true;

    float dryGain = primeSilently_ ? 0.0f : 1.0f;
    switch (state_) {
    case State::priming:
        if (primeSilently_) {
            std::memset(samples, 0, sizeof(int16_t) * size_t(count));
        }
        stateLeft_ -= count;
        if (stateLeft_ <= 0) {
            state_ = State::fadeIn;
            stateLeft_ = fadeLength_;
        }
        break;
    case State::fadeIn:
    case State::fadeOut:
        for (int i = 0; i < count; ++i) {
            float g = std::min(1.0f, float(done + i + 1) / float(fadeLength_));
            float w = state_ == State::fadeIn ? g : 1.0f - g;
            samples[i] = toInt16(softLimit(in[i] * dryGain * (1.0f - w) + wet[i] * w));
        }
        stateLeft_ -= count;
        if (stateLeft_ <= 0) {
            if (state_ == State::fadeIn) {
                state_ = State::wet;
                primeSilently_ = false;
            } else {
                state_ = State::off;
                current_ = pending_;
                latency_.store(0, std::memory_order_relaxed);
                publishInUse();
            }
        }
        break;
    case State::wet:
    case State::chainFade:
        for (int i = 0; i < count; ++i) {
            samples[i] = toInt16(wet[i]);
        }
        if (state_ == State::chainFade) {
            stateLeft_ -= count;
            if (stateLeft_ <= 0) {
                state_ = State::wet;
                publishInUse();
            }
        }
        break;
    case State::off:
        break;
    }
    return count;
}

// MARK: - Message mode

void Engine::startMessage() {
    messageStarted_ = true;
    const nvc_preset_desc &p = current_.preset;
    messageUsesTape_ = p.rate != 1.0f;
    messageUsesShifter_ = usesShifter(current_);
    lookahead_ = needsLookahead(current_) ? int(float(sampleRate_) * kLookaheadSeconds) : 0;
    if (messageUsesTape_) {
        tape_.configure(sampleRate_, p.rate, chunk_);
    }
    shifter_->reset();
    trimLeft_ = messageUsesShifter_ ? shifter_->latency() : 0;
    resetPitchControl();
    fifoStart_ = fifoLength_ = 0;
    fedToShifter_ = 0;
}

int32_t Engine::messageOutputBound(int32_t inputCount) const {
    if (inputCount < 0) {
        return NVC_ERR_ARGUMENT;
    }
    int chunks = inputCount / std::max(1, chunk_) + 2;
    return int32_t(std::ceil(double(inputCount) / kLowestRate)) + 4 * chunks + 8;
}

int Engine::messageStage(const float *input, int count, int16_t *output) {
    const EngineConfig &config = current_;
    float *y = wet_.data();
    int produced = count;
    if (messageUsesTape_) {
        produced = tape_.process(input, count, y);
    } else if (messageUsesShifter_) {
        // Align the pitch decisions with the block the engine is about to analyse.
        updatePitch(config, count, fedToShifter_ + count - shifter_->inputLatency());
        shifter_->process(input, y, count);
        fedToShifter_ += count;
        int skip = std::min(trimLeft_, count); // the engine's latency is cut, so the output lines up with the input
        trimLeft_ -= skip;
        y += skip;
        produced = count - skip;
    } else {
        std::memcpy(y, input, sizeof(float) * size_t(count));
    }
    finishWet(config, y, produced);
    for (int i = 0; i < produced; ++i) {
        output[i] = toInt16(softLimit(y[i]));
    }
    return produced;
}

int32_t Engine::processMessage(const int16_t *input, int32_t inputCount, int16_t *output, int32_t capacity) {
    if (mode_ != NVC_MODE_MESSAGE) {
        return NVC_ERR_MODE;
    }
    if (inputCount < 0 || (!input && inputCount > 0) || (!output && capacity > 0)) {
        return NVC_ERR_ARGUMENT;
    }
    if (capacity < messageOutputBound(inputCount)) {
        return NVC_ERR_CAPACITY;
    }
    if (drainPrepared_) {
        return NVC_ERR_STATE;
    }
    if (!messageStarted_) {
        std::lock_guard<std::mutex> lock(controlMutex_);
        startMessage();
    }
    if (isPassthrough(current_)) {
        // Off, or a preset that changes nothing without its stage chain (Modulation at 0): bit-identical.
        std::memcpy(output, input, sizeof(int16_t) * size_t(inputCount));
        return inputCount;
    }
    int written = 0;
    int fifoCapacity = int(fifo_.size());
    for (int pos = 0; pos < inputCount; pos += chunk_) {
        int count = std::min(chunk_, inputCount - pos);
        float *in = input_.data();
        for (int i = 0; i < count; ++i) {
            in[i] = float(input[pos + i]) * (1.0f / 32768.0f);
        }
        tracker_.push(in, count);
        publishTrackerStats();
        for (int i = 0; i < count; ++i) {
            fifo_[size_t((fifoStart_ + fifoLength_ + i) % fifoCapacity)] = in[i];
        }
        fifoLength_ += count;
        // The pitch engine runs a lookahead behind the tracker, so the speaker's pitch is known from the first word.
        while (fifoLength_ > lookahead_) {
            int take = std::min(chunk_, fifoLength_ - lookahead_);
            for (int i = 0; i < take; ++i) {
                in[i] = fifo_[size_t((fifoStart_ + i) % fifoCapacity)];
            }
            fifoStart_ = (fifoStart_ + take) % fifoCapacity;
            fifoLength_ -= take;
            written += messageStage(in, take, output + written);
        }
    }
    return written;
}

void Engine::appendDrain(const float *input, int count) {
    size_t old = drainOutput_.size();
    drainOutput_.resize(old + size_t(messageOutputBound(count)));
    int produced = messageStage(input, count, drainOutput_.data() + old);
    drainOutput_.resize(old + size_t(produced));
}

int32_t Engine::drainMessage(int16_t *output, int32_t capacity) {
    if (mode_ != NVC_MODE_MESSAGE) {
        return NVC_ERR_MODE;
    }
    if (capacity < 0 || (!output && capacity > 0)) {
        return NVC_ERR_ARGUMENT;
    }
    if (!drainPrepared_) {
        drainPrepared_ = true;
        if (!messageStarted_) {
            std::lock_guard<std::mutex> lock(controlMutex_);
            startMessage();
        }
        if (!isPassthrough(current_)) {
            int fifoCapacity = int(fifo_.size());
            float *in = input_.data();
            while (fifoLength_ > 0) {
                int take = std::min(chunk_, fifoLength_);
                for (int i = 0; i < take; ++i) {
                    in[i] = fifo_[size_t((fifoStart_ + i) % fifoCapacity)];
                }
                fifoStart_ = (fifoStart_ + take) % fifoCapacity;
                fifoLength_ -= take;
                appendDrain(in, take);
            }
            // What the pitch engine still holds comes out with as much silence as its latency.
            if (messageUsesShifter_) {
                for (int left = shifter_->latency(); left > 0; left -= chunk_) {
                    appendDrain(zeros_.data(), std::min(chunk_, left));
                }
            }
            if (messageUsesTape_) {
                appendDrain(zeros_.data(), 3);
            }
            // The chain's lag, then its ring-out (echo, reverb) faded to nothing, as on Android.
            if (current_.hasChain) {
                int lag = std::max(0, current_.chain.latency_samples);
                int tail = std::max(0, current_.chain.tail_samples);
                size_t fadeStart = drainOutput_.size() + size_t(lag);
                for (int left = lag + tail; left > 0; left -= chunk_) {
                    appendDrain(zeros_.data(), std::min(chunk_, left));
                }
                for (int i = 0; i < tail && fadeStart + size_t(i) < drainOutput_.size(); ++i) {
                    int16_t &s = drainOutput_[fadeStart + size_t(i)];
                    s = int16_t(float(s) * (1.0f - float(i) / float(tail)));
                }
            }
        }
    }
    size_t left = drainOutput_.size() - drainPos_;
    size_t n = std::min(size_t(capacity), left);
    if (n > 0) {
        std::memcpy(output, drainOutput_.data() + drainPos_, sizeof(int16_t) * n);
        drainPos_ += n;
    }
    return int32_t(n);
}

} // namespace nvc
