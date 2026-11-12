// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NVC_TAPE_RESAMPLER_HPP
#define NVC_TAPE_RESAMPLER_HPP

#include <algorithm>
#include <cmath>
#include <vector>

#include "dsp.hpp"

namespace nvc {

/// Tape speed: plays the input `rate` times faster (pitch and tempo together), so N samples become about N / rate.
/// Cubic Hermite interpolation; above rate 1 an anti-aliasing low-pass at the new Nyquist goes first.
class TapeResampler {
public:
    void configure(int sampleRate, float rate, int maxInput) {
        rate_ = rate;
        filtered_ = rate > 1.0f;
        if (filtered_) {
            antiAlias_.configure(sampleRate, 0.45 * sampleRate / rate);
        }
        buffer_.assign(size_t(maxInput) + 8, 0.0f);
        reset();
    }
    void reset() {
        antiAlias_.reset();
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        length_ = 1; // one sample of (silent) history before the first real one
        position_ = 1.0;
    }
    /// The most samples process() can return for `count` input samples.
    int bound(int count) const { return int(std::ceil(double(count) / rate_)) + 4; }

    int process(const float *input, int count, float *output) {
        for (int i = 0; i < count; ++i) {
            buffer_[size_t(length_++)] = filtered_ ? antiAlias_.process(input[i]) : input[i];
        }
        int written = 0;
        while (int(position_) + 2 < length_) {
            int i = int(position_);
            float t = float(position_ - i);
            float xm1 = buffer_[size_t(i - 1)], x0 = buffer_[size_t(i)], x1 = buffer_[size_t(i + 1)], x2 = buffer_[size_t(i + 2)];
            float c1 = 0.5f * (x1 - xm1);
            float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            output[written++] = ((c3 * t + c2) * t + c1) * t + x0;
            position_ += rate_;
        }
        // Keep one sample before the next position as history.
        int keepFrom = std::min(length_, std::max(0, int(position_) - 1));
        std::copy(buffer_.begin() + keepFrom, buffer_.begin() + length_, buffer_.begin());
        length_ -= keepFrom;
        position_ -= keepFrom;
        return written;
    }

private:
    float rate_ = 1;
    bool filtered_ = false;
    Lowpass4 antiAlias_;
    std::vector<float> buffer_;
    int length_ = 0;
    double position_ = 0;
};

} // namespace nvc

#endif
