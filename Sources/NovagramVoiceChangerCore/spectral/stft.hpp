// SPDX-License-Identifier: GPL-2.0-or-later AND MIT
//
// Derived from signalsmith-linear 0.6.4 (stft.h, DynamicSTFT), https://github.com/Signalsmith-Audio/linear,
// commit de55e6a50ffcf6f8f43f649692d94691c7025151. Adapted for Novagram: only the half-bin-shifted spectrum, Kaiser
// windows and round-trip normalisation are kept; no split computation, hooks or alternative spellings.
// Our changes are GPL-2.0-or-later; the original is MIT, and its notice is kept below as that license requires.
//
// MIT License
//
// Copyright (c) 2025 Signalsmith Audio
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef NVC_SPECTRAL_STFT_HPP
#define NVC_SPECTRAL_STFT_HPP

#include <algorithm>
#include <cstddef>
#include <vector>

#include "fft.hpp"

namespace nvc { namespace spectral {

/// A self-normalising STFT with Kaiser windows and a half-bin-shifted spectrum (see RealFft). Input goes into a ring
/// buffer, analysis windows the latest block into spectrum(channel), synthesis overlap-adds spectrum(channel) into the
/// output ring, and readOutput() divides by the summed window products, so any interval reconstructs at unit gain.
/// All memory is allocated in configure(); nothing else allocates.
template<typename Sample>
class Stft {
public:
    using Complex = std::complex<Sample>;

    void configure(size_t inChannels, size_t outChannels, size_t blockSamples, size_t extraInputHistory = 0) {
        analysisChannels_ = inChannels;
        synthesisChannels_ = outChannels;
        blockSamples_ = blockSamples;
        fftSamples_ = RealFft<Sample>::fastSizeAbove((blockSamples + 1) / 2) * 2;
        fft_.resize(fftSamples_);
        fftBins_ = fftSamples_ / 2;

        inputLengthSamples_ = blockSamples_ + extraInputHistory;
        input.buffer.resize(inputLengthSamples_ * analysisChannels_);

        output_.buffer.resize(blockSamples_ * synthesisChannels_);
        output_.windowProducts.resize(blockSamples_);
        spectrumBuffer_.resize(fftBins_ * std::max(analysisChannels_, synthesisChannels_));
        timeBuffer_.resize(std::max(fftSamples_, blockSamples_));

        analysisWindow_.resize(blockSamples_);
        synthesisWindow_.resize(blockSamples_);
        setInterval(blockSamples / 4);

        reset();
    }

    size_t blockSamples() const { return blockSamples_; }
    size_t fftSamples() const { return fftSamples_; }
    size_t defaultInterval() const { return defaultInterval_; }
    size_t bands() const { return fftBins_; }
    size_t analysisLatency() const { return blockSamples_ - analysisOffset_; }
    size_t synthesisLatency() const { return synthesisOffset_; }

    /// Bin index <-> frequency as a fraction of the sample rate (bins are half-bin shifted).
    Sample binToFreq(Sample b) const { return (b + Sample(0.5)) / fftSamples_; }
    Sample freqToBin(Sample f) const { return f * fftSamples_ - Sample(0.5); }

    void reset(Sample productWeight = 1) {
        input.pos = blockSamples_;
        output_.pos = 0;
        std::fill(input.buffer.begin(), input.buffer.end(), Sample(0));
        std::fill(output_.buffer.begin(), output_.buffer.end(), Sample(0));
        std::fill(spectrumBuffer_.begin(), spectrumBuffer_.end(), Complex(0));
        std::fill(output_.windowProducts.begin(), output_.windowProducts.end(), Sample(0));
        addWindowProduct();
        for (int i = int(blockSamples_) - int(defaultInterval_) - 1; i >= 0; --i) {
            output_.windowProducts[i] += output_.windowProducts[i + defaultInterval_];
        }
        for (auto &v : output_.windowProducts) {
            v = v * productWeight + kAlmostZero;
        }
        moveOutput(defaultInterval_); // ready for the first block immediately
    }

    void writeInput(size_t channel, size_t length, const Sample *inputArray) {
        Sample *buffer = input.buffer.data() + channel * inputLengthSamples_;
        size_t offsetPos = input.pos % inputLengthSamples_;
        size_t chunk1 = std::min(length, inputLengthSamples_ - offsetPos);
        for (size_t i = 0; i < chunk1; ++i) {
            buffer[offsetPos + i] = inputArray[i];
        }
        for (size_t i = chunk1; i < length; ++i) {
            buffer[i + offsetPos - inputLengthSamples_] = inputArray[i];
        }
    }

    void moveInput(size_t samples) {
        input.pos = (input.pos + samples) % inputLengthSamples_;
    }

    /// When no more synthesis is expected, lets the output taper away to 0 with the windows, instead of being scaled
    /// as if there were just a very long interval.
    void finishOutput(Sample strength = 1) {
        Sample maxWindowProduct = 0;
        size_t chunk1 = std::min(blockSamples_, blockSamples_ - output_.pos);
        for (size_t i = 0; i < chunk1; ++i) {
            Sample &wp = output_.windowProducts[output_.pos + i];
            maxWindowProduct = std::max(wp, maxWindowProduct);
            wp += (maxWindowProduct - wp) * strength;
        }
        for (size_t i = chunk1; i < blockSamples_; ++i) {
            Sample &wp = output_.windowProducts[i + output_.pos - blockSamples_];
            maxWindowProduct = std::max(wp, maxWindowProduct);
            wp += (maxWindowProduct - wp) * strength;
        }
    }

    void readOutput(size_t channel, size_t offset, size_t length, Sample *outputArray) const {
        const Sample *buffer = output_.buffer.data() + channel * blockSamples_;
        size_t offsetPos = (output_.pos + offset) % blockSamples_;
        size_t chunk1 = std::min(length, blockSamples_ - offsetPos);
        for (size_t i = 0; i < chunk1; ++i) {
            size_t i2 = offsetPos + i;
            outputArray[i] = buffer[i2] / output_.windowProducts[i2];
        }
        for (size_t i = chunk1; i < length; ++i) {
            size_t i2 = i + offsetPos - blockSamples_;
            outputArray[i] = buffer[i2] / output_.windowProducts[i2];
        }
    }
    void readOutput(size_t channel, size_t length, Sample *outputArray) const {
        readOutput(channel, 0, length, outputArray);
    }

    void moveOutput(size_t samples) {
        if (samples == 1) { // the common case, without the loops
            for (size_t c = 0; c < synthesisChannels_; ++c) {
                output_.buffer[output_.pos + c * blockSamples_] = 0;
            }
            output_.windowProducts[output_.pos] = kAlmostZero;
            if (++output_.pos >= blockSamples_) {
                output_.pos = 0;
            }
            return;
        }
        // Zero the output buffer as we cross it.
        size_t chunk1 = std::min(samples, blockSamples_ - output_.pos);
        for (size_t c = 0; c < synthesisChannels_; ++c) {
            Sample *buffer = output_.buffer.data() + c * blockSamples_;
            for (size_t i = 0; i < chunk1; ++i) {
                buffer[output_.pos + i] = 0;
            }
            for (size_t i = chunk1; i < samples; ++i) {
                buffer[i + output_.pos - blockSamples_] = 0;
            }
        }
        for (size_t i = 0; i < chunk1; ++i) {
            output_.windowProducts[output_.pos + i] = kAlmostZero;
        }
        for (size_t i = chunk1; i < samples; ++i) {
            output_.windowProducts[i + output_.pos - blockSamples_] = kAlmostZero;
        }
        output_.pos = (output_.pos + samples) % blockSamples_;
    }

    Complex *spectrum(size_t channel) { return spectrumBuffer_.data() + channel * fftBins_; }

    /// Kaiser analysis/synthesis windows for this interval, forced to reconstruct perfectly.
    void setInterval(size_t defaultInterval) {
        defaultInterval_ = defaultInterval;
        Kaiser window = Kaiser::withBandwidth(double(blockSamples_) / double(defaultInterval), true);
        window.fill(synthesisWindow_, blockSamples_);
        analysisOffset_ = synthesisOffset_ = blockSamples_ / 2;
        forcePerfectReconstruction(synthesisWindow_, blockSamples_, defaultInterval_);
        for (size_t i = 0; i < blockSamples_; ++i) {
            analysisWindow_[i] = synthesisWindow_[i];
        }
        // The offsets are the windows' peaks.
        for (size_t i = 0; i < blockSamples_; ++i) {
            if (analysisWindow_[i] > analysisWindow_[analysisOffset_]) analysisOffset_ = i;
            if (synthesisWindow_[i] > synthesisWindow_[synthesisOffset_]) synthesisOffset_ = i;
        }
    }

    size_t analyseSteps() const { return analysisChannels_; }

    /// Windows one channel's latest block (`samplesInPast` before the write position) into spectrum(channel).
    void analyseStep(size_t channel, size_t samplesInPast = 0) {
        size_t offsetPos = (inputLengthSamples_ * 2 + input.pos - blockSamples_ - samplesInPast) % inputLengthSamples_;
        size_t inputWrapIndex = inputLengthSamples_ - offsetPos;
        size_t chunk1 = std::min(analysisOffset_, inputWrapIndex);
        size_t chunk2 = std::max(analysisOffset_, std::min(blockSamples_, inputWrapIndex));

        size_t bufferSize = timeBuffer_.size();
        const Sample *buffer = input.buffer.data() + channel * inputLengthSamples_;
        // The half-bin shift makes the wrapped-around first half of the window negative.
        for (size_t i = 0; i < chunk1; ++i) {
            timeBuffer_[i + (bufferSize - analysisOffset_)] = buffer[offsetPos + i] * -analysisWindow_[i];
        }
        for (size_t i = chunk1; i < analysisOffset_; ++i) {
            timeBuffer_[i + (bufferSize - analysisOffset_)] = buffer[i + offsetPos - inputLengthSamples_] * -analysisWindow_[i];
        }
        for (size_t i = analysisOffset_; i < chunk2; ++i) {
            timeBuffer_[i - analysisOffset_] = buffer[offsetPos + i] * analysisWindow_[i];
        }
        for (size_t i = chunk2; i < blockSamples_; ++i) {
            timeBuffer_[i - analysisOffset_] = buffer[i + offsetPos - inputLengthSamples_] * analysisWindow_[i];
        }
        // fastSizeAbove() never gives an FFT shorter than the block, so the gap is zero padding.
        for (size_t i = blockSamples_ - analysisOffset_; i < fftSamples_ - analysisOffset_; ++i) {
            timeBuffer_[i] = 0;
        }
        fft_.fft(timeBuffer_.data(), spectrum(channel));
    }

    size_t synthesiseSteps() const { return synthesisChannels_; }

    /// Overlap-adds spectrum(channel) into the output; step 0 also adds this block's window product.
    void synthesiseStep(size_t channel) {
        if (channel == 0) {
            addWindowProduct();
        }
        fft_.ifft(spectrum(channel), timeBuffer_.data());

        Sample *buffer = output_.buffer.data() + channel * blockSamples_;
        size_t outputWrapIndex = blockSamples_ - output_.pos;
        size_t chunk1 = std::min(synthesisOffset_, outputWrapIndex);
        size_t chunk2 = std::min(blockSamples_, std::max(synthesisOffset_, outputWrapIndex));
        for (size_t i = 0; i < chunk1; ++i) {
            buffer[output_.pos + i] += timeBuffer_[i + (fftSamples_ - synthesisOffset_)] * -synthesisWindow_[i];
        }
        for (size_t i = chunk1; i < synthesisOffset_; ++i) {
            buffer[i + output_.pos - blockSamples_] += timeBuffer_[i + (fftSamples_ - synthesisOffset_)] * -synthesisWindow_[i];
        }
        for (size_t i = synthesisOffset_; i < chunk2; ++i) {
            buffer[output_.pos + i] += timeBuffer_[i - synthesisOffset_] * synthesisWindow_[i];
        }
        for (size_t i = chunk2; i < blockSamples_; ++i) {
            buffer[i + output_.pos - blockSamples_] += timeBuffer_[i - synthesisOffset_] * synthesisWindow_[i];
        }
    }

    /// The input ring, public so a caller can save and restore it (assignment copies without reallocating).
    struct Input {
        void swap(Input &other) {
            std::swap(pos, other.pos);
            std::swap(buffer, other.buffer);
        }
        Input &operator=(const Input &other) {
            pos = other.pos;
            buffer.assign(other.buffer.begin(), other.buffer.end());
            return *this;
        }

    private:
        friend class Stft;
        size_t pos = 0;
        std::vector<Sample> buffer;
    };
    Input input;

private:
    struct Output {
        size_t pos = 0;
        std::vector<Sample> buffer;
        std::vector<Sample> windowProducts;
    };
    Output output_;

    static constexpr Sample kAlmostZero = Sample(1e-30);

    RealFft<Sample> fft_;
    size_t analysisChannels_ = 0, synthesisChannels_ = 0, inputLengthSamples_ = 0, blockSamples_ = 0;
    size_t fftSamples_ = 0, fftBins_ = 0;
    size_t defaultInterval_ = 0;
    std::vector<Sample> analysisWindow_, synthesisWindow_;
    size_t analysisOffset_ = 0, synthesisOffset_ = 0;
    std::vector<Complex> spectrumBuffer_;
    std::vector<Sample> timeBuffer_;

    // Round-trip normalisation: the analysis window times the synthesis window, scaled by the FFT size.
    void addWindowProduct() {
        int windowShift = int(synthesisOffset_) - int(analysisOffset_);
        size_t wMin = size_t(std::max<ptrdiff_t>(0, windowShift));
        size_t wMax = size_t(std::min<ptrdiff_t>(ptrdiff_t(blockSamples_), ptrdiff_t(blockSamples_) + windowShift));
        Sample *windowProduct = output_.windowProducts.data();
        size_t outputWrapIndex = blockSamples_ - output_.pos;
        size_t chunk1 = std::min<size_t>(wMax, std::max<size_t>(wMin, outputWrapIndex));
        Sample scaling = Sample(fftSamples_);
        for (size_t i = wMin; i < chunk1; ++i) {
            windowProduct[output_.pos + i] += analysisWindow_[i - windowShift] * synthesisWindow_[i] * scaling;
        }
        for (size_t i = chunk1; i < wMax; ++i) {
            windowProduct[i + output_.pos - blockSamples_] += analysisWindow_[i - windowShift] * synthesisWindow_[i] * scaling;
        }
    }

    class Kaiser {
    public:
        static Kaiser withBandwidth(double bandwidth, bool heuristicOptimal) {
            return Kaiser(bandwidthToBeta(bandwidth, heuristicOptimal));
        }
        static double bandwidthToBeta(double bandwidth, bool heuristicOptimal) {
            if (heuristicOptimal) { // heuristic from a numerical search
                bandwidth = bandwidth + 8 / ((bandwidth + 3) * (bandwidth + 3)) + 0.25 * std::max(3 - bandwidth, 0.0);
            }
            bandwidth = std::max(bandwidth, 2.0);
            double alpha = std::sqrt(bandwidth * bandwidth * 0.25 - 1);
            return alpha * M_PI;
        }
        explicit Kaiser(double beta) : beta_(beta), invB0_(1 / bessel0(beta)) {}

        /// A synthesis window of `size` samples.
        void fill(std::vector<Sample> &data, size_t size) const {
            double invSize = 1.0 / double(size);
            size_t offsetI = (size & 1) ? 1 : 0;
            for (size_t i = 0; i < size; ++i) {
                double r = double(2 * i + offsetI) * invSize - 1;
                double arg = std::sqrt(1 - r * r);
                data[i] = Sample(bessel0(beta_ * arg) * invB0_);
            }
        }

    private:
        static double bessel0(double x) {
            const double significanceLimit = 1e-4;
            double result = 0;
            double term = 1;
            double m = 0;
            while (term > significanceLimit) {
                result += term;
                ++m;
                term *= (x * x) / (4 * m * m);
            }
            return result;
        }
        double beta_;
        double invB0_;
    };

    static void forcePerfectReconstruction(std::vector<Sample> &data, size_t windowLength, size_t interval) {
        for (size_t i = 0; i < interval; ++i) {
            double sum2 = 0;
            for (size_t index = i; index < windowLength; index += interval) {
                sum2 += double(data[index]) * double(data[index]);
            }
            double factor = 1 / std::sqrt(sum2);
            for (size_t index = i; index < windowLength; index += interval) {
                data[index] = Sample(data[index] * factor);
            }
        }
    }
};

}} // namespace nvc::spectral

#endif
