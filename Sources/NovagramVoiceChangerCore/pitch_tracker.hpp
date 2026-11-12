// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NVC_PITCH_TRACKER_HPP
#define NVC_PITCH_TRACKER_HPP

#include <cstdint>
#include <vector>

#include "dsp.hpp"

namespace nvc {

/// Real-time YIN pitch tracker (de Cheveigne & Kawahara, 2002) for one voice.
///
/// The input is low-passed at 1 kHz and decimated to about 8 kHz, so a 10 ms frame costs about 18k multiply-adds at any
/// sample rate. Each frame gives a raw estimate and a voiced/unvoiced decision (periodicity plus a level gate); the
/// smoothed pitch is the median of the voiced estimates among the last 5 frames, and the speaker's pitch is the median
/// of the last 3 s of voiced frames, which moves slowly enough to steer a pitch shift without warbling.
/// configure() allocates; push() never allocates or locks.
class PitchTracker {
public:
    static constexpr float kMinF0 = 60.0f;
    static constexpr float kMaxF0 = 900.0f;
    static constexpr float kThreshold = 0.15f;      ///< YIN's absolute threshold on the normalised difference.
    static constexpr float kGate = 0.003f;          ///< Frames quieter than about -50 dBFS RMS are unvoiced.
    static constexpr int kSmoothFrames = 5;
    static constexpr int kSpeakerFrames = 300;      ///< 3 s of voicing.
    static constexpr int kMinSpeakerFrames = 20;    ///< 0.2 s of voicing before the speaker's pitch counts.
    static constexpr int kHistory = 512;            ///< Frames kept for smoothedF0At() (5 s).

    void configure(int sampleRate);
    void reset();
    void push(const float *samples, int count);

    bool voiced() const { return voiced_; }
    float rawF0() const { return rawF0_; }
    /// The median of the recent voiced estimates, 0 when the latest frame is unvoiced.
    float smoothedF0() const { return smoothedF0_; }
    /// 0 until kMinSpeakerFrames voiced frames were heard.
    float speakerF0() const { return speakerF0_; }
    int64_t voicedFrames() const { return voicedFrames_; }
    int64_t frames() const { return frames_; }
    /// The smoothed pitch of the frame centred closest to input sample `sample` (0 if unvoiced or not analysed yet).
    float smoothedF0At(int64_t sample) const;

private:
    void pushDecimated(float x);
    void analyse();

    int decimation_ = 1;
    float analysisRate_ = 8000;
    int tauMin_ = 2, tauMax_ = 2, window_ = 2, bufferLength_ = 4, hop_ = 80;
    Lowpass4 lowpass_;
    int decimationPhase_ = 0;
    int64_t inputSamples_ = 0;

    std::vector<float> ring_, frame_, difference_, normalised_;
    int writePos_ = 0, filled_ = 0, sinceHop_ = 0;

    bool voiced_ = false;
    float rawF0_ = 0, smoothedF0_ = 0, speakerF0_ = 0;
    float recent_[kSmoothFrames] = {};
    int recentPos_ = 0;

    std::vector<float> speakerLog2_, speakerScratch_;
    int speakerPos_ = 0, speakerCount_ = 0;
    int64_t voicedFrames_ = 0, frames_ = 0;

    struct HistoryEntry {
        int64_t centre;
        float f0;
    };
    std::vector<HistoryEntry> history_;
};

} // namespace nvc

#endif
