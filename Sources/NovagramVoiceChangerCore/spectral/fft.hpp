// SPDX-License-Identifier: GPL-2.0-or-later AND MIT
//
// Derived from signalsmith-linear 0.6.4 (fft.h, the half-bin-shifted RealFFT), https://github.com/Signalsmith-Audio/linear,
// commit de55e6a50ffcf6f8f43f649692d94691c7025151. Rewritten for Novagram: power-of-two sizes only, our own radix-2
// complex FFT, no split computation, no SIMD or platform back-ends.
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

#ifndef NVC_SPECTRAL_FFT_HPP
#define NVC_SPECTRAL_FFT_HPP

#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace nvc { namespace spectral {

/// Smallest power of two that is at least `size` (and at least 1).
inline size_t nextPowerOfTwo(size_t size) {
    size_t result = 1;
    while (result < size) {
        result <<= 1;
    }
    return result;
}

/// Iterative radix-2 complex FFT of a power-of-two size. Unscaled both ways: ifft(fft(x)) == size * x.
/// Memory is allocated in resize() only.
template<typename Sample>
class ComplexFft {
public:
    using Complex = std::complex<Sample>;

    void resize(size_t size) {
        size_ = size;
        twiddles_.resize(size / 2);
        for (size_t i = 0; i < twiddles_.size(); ++i) {
            twiddles_[i] = std::polar(Sample(1), Sample(-2 * M_PI * double(i) / double(size)));
        }
        reversed_.resize(size);
        size_t bits = 0;
        while ((size_t(1) << bits) < size) {
            ++bits;
        }
        for (size_t i = 0; i < size; ++i) {
            size_t r = 0;
            for (size_t b = 0; b < bits; ++b) {
                if (i & (size_t(1) << b)) {
                    r |= size_t(1) << (bits - 1 - b);
                }
            }
            reversed_[i] = r;
        }
    }

    size_t size() const {
        return size_;
    }

    /// `input` and `output` must not overlap.
    void fft(const Complex *input, Complex *output) const {
        transform(input, output, false);
    }

    void ifft(const Complex *input, Complex *output) const {
        transform(input, output, true);
    }

private:
    void transform(const Complex *input, Complex *output, bool inverse) const {
        for (size_t i = 0; i < size_; ++i) {
            output[reversed_[i]] = input[i];
        }
        // Plain multiplications: std::complex's operator* checks for NaN/Inf without -ffast-math and is much slower.
        for (size_t length = 2; length <= size_; length <<= 1) {
            size_t half = length / 2;
            size_t step = size_ / length;
            for (size_t start = 0; start < size_; start += length) {
                for (size_t k = 0; k < half; ++k) {
                    const Complex &w = twiddles_[k * step];
                    Sample wr = w.real();
                    Sample wi = inverse ? -w.imag() : w.imag();
                    Complex &a = output[start + k];
                    Complex &b = output[start + k + half];
                    Sample br = b.real() * wr - b.imag() * wi;
                    Sample bi = b.real() * wi + b.imag() * wr;
                    Sample ar = a.real(), ai = a.imag();
                    a = Complex(ar + br, ai + bi);
                    b = Complex(ar - br, ai - bi);
                }
            }
        }
    }

    size_t size_ = 0;
    std::vector<Complex> twiddles_;
    std::vector<size_t> reversed_;
};

/// Real FFT whose bins sit half a bin up: bin k is the frequency (k + 0.5) / size, for k in [0, size/2).
/// This "modified" spectrum has no special DC/Nyquist bins, which keeps the phase vocoder simple.
/// A size/2 complex FFT does the work; the twist before it and the twiddles after it follow signalsmith-linear.
template<typename Sample>
class RealFft {
public:
    using Complex = std::complex<Sample>;

    /// The FFT size used for at least `size` samples (a power of two, at least 4).
    static size_t fastSizeAbove(size_t size) {
        size_t half = nextPowerOfTwo((size + 1) / 2);
        return (half < 2 ? 2 : half) * 2;
    }

    void resize(size_t size) {
        size_t halfSize = size / 2;
        complexFft_.resize(halfSize);
        tmpTime_.resize(halfSize);
        tmpFreq_.resize(halfSize);
        twiddles_.resize(halfSize / 2 + 1);
        for (size_t i = 0; i < twiddles_.size(); ++i) {
            double phase = (double(i) + 0.5) * (-2 * M_PI / double(size)) - M_PI / 2;
            twiddles_[i] = std::polar(Sample(1), Sample(phase));
        }
        halfBinTwists_.resize(halfSize);
        for (size_t i = 0; i < halfSize; ++i) {
            halfBinTwists_[i] = std::polar(Sample(1), Sample(-2 * M_PI * double(i) / double(size)));
        }
    }

    size_t size() const {
        return complexFft_.size() * 2;
    }

    /// `time` has size() samples, `freq` size()/2 bins.
    void fft(const Sample *time, Complex *freq) {
        size_t halfSize = complexFft_.size();
        for (size_t i = 0; i < halfSize; ++i) {
            Sample tr = time[2 * i], ti = time[2 * i + 1];
            const Complex &twist = halfBinTwists_[i];
            tmpTime_[i] = Complex(tr * twist.real() - ti * twist.imag(), ti * twist.real() + tr * twist.imag());
        }
        complexFft_.fft(tmpTime_.data(), tmpFreq_.data());
        for (size_t i = 0; i < halfSize / 2 + 1; ++i) {
            size_t conjI = halfSize - 1 - i;
            const Complex &twiddle = twiddles_[i];
            Complex a = tmpFreq_[i], b = std::conj(tmpFreq_[conjI]);
            Complex odd((a.real() + b.real()) * Sample(0.5), (a.imag() + b.imag()) * Sample(0.5));
            Complex evenI((a.real() - b.real()) * Sample(0.5), (a.imag() - b.imag()) * Sample(0.5));
            // The twiddle includes a factor of -i.
            Complex evenRotMinusI(evenI.real() * twiddle.real() - evenI.imag() * twiddle.imag(),
                                  evenI.imag() * twiddle.real() + evenI.real() * twiddle.imag());
            freq[i] = Complex(odd.real() + evenRotMinusI.real(), odd.imag() + evenRotMinusI.imag());
            freq[conjI] = Complex(odd.real() - evenRotMinusI.real(), evenRotMinusI.imag() - odd.imag());
        }
    }

    /// Unscaled inverse of fft(): ifft(fft(x)) == size() * x.
    void ifft(const Complex *freq, Sample *time) {
        size_t halfSize = complexFft_.size();
        for (size_t i = 0; i < halfSize / 2 + 1; ++i) {
            size_t conjI = halfSize - 1 - i;
            const Complex &twiddle = twiddles_[i];
            Complex a = freq[i], b = std::conj(freq[conjI]);
            Complex odd(a.real() + b.real(), a.imag() + b.imag());
            Complex evenRotMinusI(a.real() - b.real(), a.imag() - b.imag());
            // Conjugate twiddle.
            Complex evenI(evenRotMinusI.real() * twiddle.real() + evenRotMinusI.imag() * twiddle.imag(),
                          evenRotMinusI.imag() * twiddle.real() - evenRotMinusI.real() * twiddle.imag());
            tmpFreq_[i] = Complex(odd.real() + evenI.real(), odd.imag() + evenI.imag());
            tmpFreq_[conjI] = Complex(odd.real() - evenI.real(), evenI.imag() - odd.imag());
        }
        complexFft_.ifft(tmpFreq_.data(), tmpTime_.data());
        for (size_t i = 0; i < halfSize; ++i) {
            const Complex &t = tmpTime_[i];
            const Complex &twist = halfBinTwists_[i];
            time[2 * i] = t.real() * twist.real() + t.imag() * twist.imag();
            time[2 * i + 1] = t.imag() * twist.real() - t.real() * twist.imag();
        }
    }

private:
    ComplexFft<Sample> complexFft_;
    std::vector<Complex> tmpTime_, tmpFreq_;
    std::vector<Complex> twiddles_, halfBinTwists_;
};

}} // namespace nvc::spectral

#endif
