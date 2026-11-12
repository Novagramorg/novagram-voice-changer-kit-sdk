// SPDX-License-Identifier: GPL-2.0-or-later
#include "pitch_tracker.hpp"

#include <algorithm>
#include <cmath>

namespace nvc {

void PitchTracker::configure(int sampleRate) {
    decimation_ = std::max(1, sampleRate / 8000);
    analysisRate_ = float(sampleRate) / float(decimation_);
    lowpass_.configure(sampleRate, std::min(1000.0, 0.45 * analysisRate_ / 2));
    tauMax_ = int(std::ceil(analysisRate_ / kMinF0));
    tauMin_ = std::max(2, int(std::floor(analysisRate_ / kMaxF0)));
    window_ = tauMax_;
    bufferLength_ = window_ + tauMax_ + 1;
    hop_ = std::max(1, int(std::lround(analysisRate_ * 0.01)));
    ring_.assign(size_t(bufferLength_), 0.0f);
    frame_.assign(size_t(bufferLength_), 0.0f);
    difference_.assign(size_t(tauMax_ + 2), 0.0f);
    normalised_.assign(size_t(tauMax_ + 2), 1.0f);
    speakerLog2_.assign(kSpeakerFrames, 0.0f);
    speakerScratch_.assign(kSpeakerFrames, 0.0f);
    history_.assign(kHistory, HistoryEntry{0, 0.0f});
    reset();
}

void PitchTracker::reset() {
    lowpass_.reset();
    decimationPhase_ = 0;
    inputSamples_ = 0;
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    writePos_ = filled_ = sinceHop_ = 0;
    voiced_ = false;
    rawF0_ = smoothedF0_ = speakerF0_ = 0;
    std::fill(recent_, recent_ + kSmoothFrames, 0.0f);
    recentPos_ = 0;
    speakerPos_ = speakerCount_ = 0;
    voicedFrames_ = frames_ = 0;
}

void PitchTracker::push(const float *samples, int count) {
    for (int i = 0; i < count; ++i) {
        float y = lowpass_.process(samples[i]);
        ++inputSamples_;
        if (++decimationPhase_ >= decimation_) {
            decimationPhase_ = 0;
            pushDecimated(y);
        }
    }
}

void PitchTracker::pushDecimated(float x) {
    ring_[size_t(writePos_)] = x;
    writePos_ = (writePos_ + 1) % bufferLength_;
    if (filled_ < bufferLength_) {
        ++filled_;
    }
    if (++sinceHop_ >= hop_ && filled_ == bufferLength_) {
        sinceHop_ = 0;
        analyse();
    }
}

void PitchTracker::analyse() {
    // Oldest sample first.
    for (int i = 0; i < bufferLength_; ++i) {
        frame_[size_t(i)] = ring_[size_t((writePos_ + i) % bufferLength_)];
    }
    double energy = 0;
    for (int i = 0; i < bufferLength_; ++i) {
        energy += double(frame_[size_t(i)]) * frame_[size_t(i)];
    }
    float rms = float(std::sqrt(energy / bufferLength_));

    // Difference function and its cumulative-mean normalisation.
    const float *x = frame_.data();
    float running = 0;
    normalised_[0] = 1;
    for (int tau = 1; tau <= tauMax_; ++tau) {
        float sum = 0;
        for (int j = 0; j < window_; ++j) {
            float d = x[j] - x[j + tau];
            sum += d * d;
        }
        difference_[size_t(tau)] = sum;
        running += sum;
        normalised_[size_t(tau)] = running > 0 ? sum * float(tau) / running : 1.0f;
    }

    // The first dip under the threshold, followed down to its minimum; else the global minimum (aperiodic).
    int best = -1;
    for (int tau = tauMin_; tau <= tauMax_; ++tau) {
        if (normalised_[size_t(tau)] < kThreshold) {
            while (tau + 1 <= tauMax_ && normalised_[size_t(tau + 1)] < normalised_[size_t(tau)]) {
                ++tau;
            }
            best = tau;
            break;
        }
    }
    bool periodic = best > 0;
    if (!periodic) {
        best = tauMin_;
        for (int tau = tauMin_ + 1; tau <= tauMax_; ++tau) {
            if (normalised_[size_t(tau)] < normalised_[size_t(best)]) {
                best = tau;
            }
        }
    }
    float period = float(best);
    if (best > tauMin_ && best < tauMax_) {
        float a = normalised_[size_t(best - 1)], b = normalised_[size_t(best)], c = normalised_[size_t(best + 1)];
        float denominator = a - 2 * b + c;
        if (denominator > 0) {
            period += 0.5f * (a - c) / denominator;
        }
    }
    rawF0_ = analysisRate_ / period;
    voiced_ = periodic && rms > kGate && rawF0_ >= kMinF0 && rawF0_ <= kMaxF0;

    // Median of the voiced estimates among the last few frames.
    recent_[recentPos_] = voiced_ ? rawF0_ : 0.0f;
    recentPos_ = (recentPos_ + 1) % kSmoothFrames;
    smoothedF0_ = 0;
    if (voiced_) {
        float values[kSmoothFrames];
        int n = 0;
        for (float v : recent_) {
            if (v > 0) {
                values[n++] = v;
            }
        }
        std::sort(values, values + n);
        smoothedF0_ = (n % 2) ? values[n / 2] : 0.5f * (values[n / 2 - 1] + values[n / 2]);
    }

    // The speaker: a slow median of voiced frames, in the log domain.
    if (voiced_) {
        ++voicedFrames_;
        speakerLog2_[size_t(speakerPos_)] = std::log2(smoothedF0_);
        speakerPos_ = (speakerPos_ + 1) % kSpeakerFrames;
        speakerCount_ = std::min(speakerCount_ + 1, kSpeakerFrames);
        if (speakerCount_ >= kMinSpeakerFrames) {
            std::copy(speakerLog2_.begin(), speakerLog2_.begin() + speakerCount_, speakerScratch_.begin());
            auto middle = speakerScratch_.begin() + speakerCount_ / 2;
            std::nth_element(speakerScratch_.begin(), middle, speakerScratch_.begin() + speakerCount_);
            speakerF0_ = std::min(500.0f, std::max(kMinF0, std::exp2(*middle)));
        }
    }

    int64_t centre = inputSamples_ - int64_t(bufferLength_ / 2) * decimation_;
    history_[size_t(frames_ % kHistory)] = HistoryEntry{centre, smoothedF0_};
    ++frames_;
}

float PitchTracker::smoothedF0At(int64_t sample) const {
    if (frames_ == 0) {
        return 0;
    }
    int64_t newest = frames_ - 1;
    int64_t oldest = std::max<int64_t>(0, frames_ - kHistory);
    int64_t frameSamples = int64_t(hop_) * decimation_;
    int64_t newestCentre = history_[size_t(newest % kHistory)].centre;
    int64_t back = (newestCentre - sample + frameSamples / 2) / frameSamples;
    int64_t index = std::min(newest, std::max(oldest, newest - std::max<int64_t>(0, back)));
    return history_[size_t(index % kHistory)].f0;
}

} // namespace nvc
