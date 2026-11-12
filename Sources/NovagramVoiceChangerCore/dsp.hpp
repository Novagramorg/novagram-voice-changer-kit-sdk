// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NVC_DSP_HPP
#define NVC_DSP_HPP

#include <atomic>
#include <cmath>
#include <cstdint>

namespace nvc {

constexpr double kPi = 3.14159265358979323846;

/// RBJ-cookbook biquad, transposed direct form II.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void setLowpass(double sampleRate, double cutoff, double q) {
        double w = 2 * kPi * cutoff / sampleRate;
        double alpha = std::sin(w) / (2 * q);
        double cosW = std::cos(w);
        double a0 = 1 + alpha;
        b0 = float((1 - cosW) / 2 / a0);
        b1 = float((1 - cosW) / a0);
        b2 = b0;
        a1 = float(-2 * cosW / a0);
        a2 = float((1 - alpha) / a0);
    }
    void reset() { z1 = z2 = 0; }
    float process(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

/// Fourth-order Butterworth low-pass (two biquads).
struct Lowpass4 {
    Biquad first, second;
    void configure(double sampleRate, double cutoff) {
        first.setLowpass(sampleRate, cutoff, 0.54119610);
        second.setLowpass(sampleRate, cutoff, 1.30656296);
    }
    void reset() {
        first.reset();
        second.reset();
    }
    float process(float x) { return second.process(first.process(x)); }
};

/// Linear up to 0.9, then bends smoothly toward 1, so loud peaks never clip hard (Android's softLimit).
inline float softLimit(float x) {
    float a = std::fabs(x);
    if (a <= 0.9f) {
        return x;
    }
    float bent = 0.9f + 0.1f * std::tanh((a - 0.9f) / 0.1f);
    return x < 0 ? -bent : bent;
}

/// A limited float in -1..1 to 16 bits. Never full scale: tanh in softLimit rounds to exactly 1 for very loud input.
inline int16_t toInt16(float x) {
    float v = x * 32767.0f;
    if (v > 32766.0f) v = 32766.0f;
    if (v < -32766.0f) v = -32766.0f;
    return int16_t(v);
}

/// One-pole smoothing coefficient for a time constant (0 = jump).
inline float smoothing(float dt, float tau) {
    return tau <= 0 ? 1.0f : 1.0f - std::exp(-dt / tau);
}

/// Lock-free "latest value" mailbox between one writer thread and one reader thread (a triple buffer). The writer fills
/// back() and publishes; the reader's update() swaps in the newest published value, if any. No allocation, no locks.
template<typename T>
class TripleBuffer {
public:
    T &back() { return slots_[back_]; }
    void publish() {
        int previous = middle_.exchange(back_ | kDirty, std::memory_order_acq_rel);
        back_ = previous & kIndexMask;
    }
    bool update() {
        if ((middle_.load(std::memory_order_acquire) & kDirty) == 0) {
            return false;
        }
        int previous = middle_.exchange(front_, std::memory_order_acq_rel);
        front_ = previous & kIndexMask;
        return true;
    }
    const T &front() const { return slots_[front_]; }

private:
    static constexpr int kDirty = 4;
    static constexpr int kIndexMask = 3;
    T slots_[3] = {};
    int back_ = 0;
    std::atomic<int> middle_{1};
    int front_ = 2;
};

} // namespace nvc

#endif
