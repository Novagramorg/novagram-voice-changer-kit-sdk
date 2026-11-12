// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NVC_ENGINE_HPP
#define NVC_ENGINE_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "dsp.hpp"
#include "nvc.h"
#include "pitch_tracker.hpp"
#include "tape_resampler.hpp"

namespace nvc {

class PitchShifter;

/// One preset choice as the audio thread sees it. Plain data: it crosses threads by copy.
struct EngineConfig {
    nvc_preset_desc preset;
    nvc_params params;
    nvc_stage_chain chain;
    bool hasChain;
    uint64_t generation;
};

/// The voice changer: YIN pitch tracker -> pitch/formant engine (or tape speed) -> stage chain -> gain -> soft limit.
class Engine {
public:
    Engine(nvc_mode mode, int sampleRate, int maxBlock);
    ~Engine();

    nvc_mode mode() const { return mode_; }
    int32_t setPreset(nvc_preset_id id, const nvc_params *params, const nvc_stage_chain *chain);
    int32_t latencySamples() const { return latency_.load(std::memory_order_relaxed); }
    float speakerPitchHz() const { return speakerPitch_.load(std::memory_order_relaxed); }
    float shiftSemitones() const { return shift_.load(std::memory_order_relaxed); }

    int32_t processCall(int16_t *samples, int32_t count);

    int32_t messageOutputBound(int32_t inputCount) const;
    int32_t processMessage(const int16_t *input, int32_t inputCount, int16_t *output, int32_t capacity);
    int32_t drainMessage(int16_t *output, int32_t capacity);

private:
    enum class State { off, priming, fadeIn, wet, chainFade, fadeOut };

    // Control side.
    void releaseRetiredChains(bool all);
    std::mutex controlMutex_;
    TripleBuffer<EngineConfig> mailbox_;
    uint64_t nextGeneration_ = 1;
    std::vector<std::pair<uint64_t, nvc_stage_chain>> ownedChains_;
    std::atomic<uint64_t> oldestInUse_{0};

    // Shared pieces.
    bool usesShifter(const EngineConfig &config) const;
    bool needsLookahead(const EngineConfig &config) const;
    bool isPassthrough(const EngineConfig &config) const;
    int32_t chainLatency(const EngineConfig &config) const;
    void resetPitchControl();
    void updatePitch(const EngineConfig &config, int count, int64_t alignSample);
    void finishWet(const EngineConfig &config, float *samples, int count);
    void publishTrackerStats();

    // Call mode.
    void pickUpConfig();
    void publishInUse();

    // Message mode.
    void startMessage();
    int messageStage(const float *input, int count, int16_t *output);
    void appendDrain(const float *input, int count);

    nvc_mode mode_;
    int sampleRate_;
    int maxBlock_;
    int chunk_;       // message mode: samples per internal step
    int fadeLength_;  // call mode crossfades

    PitchTracker tracker_;
    std::unique_ptr<PitchShifter> shifter_;
    TapeResampler tape_;
    std::vector<float> input_, wet_, wetOld_, zeros_;

    EngineConfig current_{}, previous_{}, pending_{};
    State state_ = State::off;
    int stateLeft_ = 0;

    float appliedShift_ = 0, heldShift_ = 0;
    bool shiftValid_ = false, speakerKnown_ = false;
    float fastGlideLeft_ = 0;
    float autotuneNote_ = -1;
    bool primeSilently_ = false, hasProcessed_ = false;

    std::atomic<int32_t> latency_{0};
    std::atomic<float> speakerPitch_{0}, shift_{0};

    // Message mode state.
    bool messageStarted_ = false, messageUsesShifter_ = false, messageUsesTape_ = false;
    int lookahead_ = 0, trimLeft_ = 0;
    std::vector<float> fifo_;
    int fifoStart_ = 0, fifoLength_ = 0;
    int64_t fedToShifter_ = 0;
    bool drainPrepared_ = false;
    std::vector<int16_t> drainOutput_;
    size_t drainPos_ = 0;
};

} // namespace nvc

#endif
