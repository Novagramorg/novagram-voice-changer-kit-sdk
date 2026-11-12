// SPDX-License-Identifier: GPL-2.0-or-later AND MIT
//
// Derived from Signalsmith Stretch 1.4.0 (signalsmith-stretch.h), https://github.com/Signalsmith-Audio/signalsmith-stretch,
// commit a670068d9aeb64913331d5cc29337b19a457a7df. Adapted for Novagram: our namespace and names, a seeded random
// engine, and no seeking, custom frequency maps, split computation or profiling hooks.
// Our changes are GPL-2.0-or-later; the original is MIT, and its notice is kept below as that license requires.
//
// MIT License
//
// Copyright (c) 2022 Geraint Luff / Signalsmith Audio Ltd.
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

#ifndef NVC_SPECTRAL_STRETCHER_HPP
#define NVC_SPECTRAL_STRETCHER_HPP

#include "stft.hpp"

#include <vector>
#include <array>
#include <algorithm>
#include <random>
#include <limits>
#include <type_traits>

namespace nvc { namespace spectral {

namespace stretcher_impl {
    template<bool conjugateSecond=false, typename V>
    static std::complex<V> mul(const std::complex<V> &a, const std::complex<V> &b) {
        return conjugateSecond ? std::complex<V>{
            b.real()*a.real() + b.imag()*a.imag(),
                b.real()*a.imag() - b.imag()*a.real()
        } : std::complex<V>{
            a.real()*b.real() - a.imag()*b.imag(),
            a.real()*b.imag() + a.imag()*b.real()
        };
    }
    template<typename V>
    static V norm(const std::complex<V> &a) {
        V r = a.real(), i = a.imag();
        return r*r + i*i;
    }
}

/// Pitch and time stretching with formant control (the phase-vocoder approach of Signalsmith Stretch).
/// configure() allocates; process() and flush() don't.
template<typename Sample=float>
class Stretcher {
public:

    /// Seeded, so that the same input always gives the same output.
    explicit Stretcher(unsigned int seed = 0x4e56u) : randomEngine(seed) {}
        
    // The difference between the internal position (centre of a block) and the input samples you're supplying
    int inputLatency() const {
        return int(stft.analysisLatency());
    }
    int outputLatency() const {
        return int(stft.synthesisLatency());
    }
    
    void reset() {
        stft.reset(0.1);
        stashedInput = stft.input;
        
        prevInputOffset = -1;
        _channelBands.assign(_channelBands.size(), Band());
        silenceCounter = 0;
        blockProcess = {};
        freqEstimateWeighted = freqEstimateWeight = 0;
    }

    // Manual setup
    void configure(int nChannels, int blockSamples, int intervalSamples) {
        channels = nChannels;
        stft.configure(channels, channels, blockSamples, intervalSamples + 1);
        stft.setInterval(intervalSamples);
        stft.reset(Sample(0.1));
        stashedInput = stft.input;

        bands = int(stft.bands());
        _channelBands.assign(bands*channels, Band());
        
        peaks.reserve(bands/2);
        energy.resize(bands);
        smoothedEnergy.resize(bands);
        outputMap.resize(bands);
        channelPredictions.resize(channels*bands);

        blockProcess = {};
        formantMetric.resize(bands + 2);

        tmpProcessBuffer.resize(blockSamples + intervalSamples);
    }
    // For querying the existing config
    int blockSamples() const {
        return int(stft.blockSamples());
    }
    int intervalSamples() const {
        return int(stft.defaultInterval());
    }

    /// Frequency multiplier, and optional tonality limit (as multiple of sample-rate)
    void setTransposeFactor(Sample multiplier, Sample tonalityLimit=0) {
        freqMultiplier = multiplier;
        if (tonalityLimit > 0) {
            freqTonalityLimit = tonalityLimit/std::sqrt(multiplier); // compromise between input and output limits
        } else {
            freqTonalityLimit = 1;
        }
    }
    void setTransposeSemitones(Sample semitones, Sample tonalityLimit=0) {
        setTransposeFactor(std::pow(2, semitones/12), tonalityLimit);
    }

    void setFormantFactor(Sample multiplier, bool compensatePitch=false) {
        formantMultiplier = multiplier;
        invFormantMultiplier = 1/multiplier;
        formantCompensation = compensatePitch;
    }
    void setFormantSemitones(Sample semitones, bool compensatePitch=false) {
        setFormantFactor(std::pow(2, semitones/12), compensatePitch);
    }
    // Rough guesstimate of the fundamental frequency, used for formant analysis. 0 means attempting to detect the pitch
    void setFormantBase(Sample baseFreq=0) {
        formantBaseFreq = baseFreq;
    }
    
    template<class Inputs, class Outputs>
    void process(Inputs &&inputs, int inputSamples, Outputs &&outputs, int outputSamples) {
        int prevCopiedInput = 0;
        auto copyInput = [&](int toIndex){

            int length = std::min<int>(int(stft.blockSamples() + stft.defaultInterval()), toIndex - prevCopiedInput);
            tmpProcessBuffer.resize(length);
            int offset = toIndex - length;
            for (int c = 0; c < channels; ++c) {
                auto &&inputBuffer = inputs[c];
                for (int i = 0; i < length; ++i) {
                    tmpProcessBuffer[i] = inputBuffer[i + offset];
                }
                stft.writeInput(c, length, tmpProcessBuffer.data());
            }
            stft.moveInput(length);
            prevCopiedInput = toIndex;
        };

        Sample totalEnergy = 0;
        for (int c = 0; c < channels; ++c) {
            auto &&inputChannel = inputs[c];
            for (int i = 0; i < inputSamples; ++i) {
                Sample s = inputChannel[i];
                totalEnergy += s*s;
            }
        }

        if (totalEnergy < noiseFloor) {
            if (silenceCounter >= 2*stft.blockSamples()) {
                if (silenceFirst) { // first block of silence processing
                    silenceFirst = false;
                    //stft.reset();
                    blockProcess = {};
                    for (auto &b : _channelBands) {
                        b.input = b.prevInput = b.output = 0;
                        b.inputEnergy = 0;
                    }
                }
            
                if (inputSamples > 0) {
                    // copy from the input, wrapping around if needed
                    for (int outputIndex = 0; outputIndex < outputSamples; ++outputIndex) {
                        int inputIndex = outputIndex%inputSamples;
                        for (int c = 0; c < channels; ++c) {
                            outputs[c][outputIndex] = inputs[c][inputIndex];
                        }
                    }
                } else {
                    for (int c = 0; c < channels; ++c) {
                        auto &&outputChannel = outputs[c];
                        for (int outputIndex = 0; outputIndex < outputSamples; ++outputIndex) {
                            outputChannel[outputIndex] = 0;
                        }
                    }
                }

                // Store input in history buffer
                copyInput(inputSamples);
                return;
            } else {
                silenceCounter += inputSamples;
            }
        } else {
            silenceCounter = 0;
            silenceFirst = true;
        }
        
        for (int outputIndex = 0; outputIndex < outputSamples; ++outputIndex) {
            bool newBlock = blockProcess.samplesSinceLast >= stft.defaultInterval();
            if (newBlock) {
                blockProcess.step = 0;
                blockProcess.steps = 0; // how many processing steps this block will have
                blockProcess.samplesSinceLast = 0;
                
                // Time to process a spectrum!  Where should it come from in the input?
                int inputOffset = static_cast<int>(std::round(outputIndex*Sample(inputSamples)/outputSamples));
                int inputInterval = inputOffset - prevInputOffset;
                prevInputOffset = inputOffset;
                
                copyInput(inputOffset);
                stashedInput = stft.input; // save the input state, since that's what we'll analyse later

                blockProcess.newSpectrum = (inputInterval > 0);
                blockProcess.mappedFrequencies = freqMultiplier != 1;
                if (blockProcess.newSpectrum) {
                    // make sure the previous input is the correct distance in the past (give or take 1 sample)
                    blockProcess.reanalysePrev = std::abs(inputInterval - int(stft.defaultInterval())) > 1;
                    if (blockProcess.reanalysePrev) blockProcess.steps += stft.analyseSteps() + 1;

                    // analyse a new input
                    blockProcess.steps += stft.analyseSteps() + 1;
                }
                
                blockProcess.processFormants = formantMultiplier != 1 || (formantCompensation && blockProcess.mappedFrequencies);

                blockProcess.timeFactor = stft.defaultInterval()/static_cast<Sample>(std::max(1, inputInterval));

                updateProcessSpectrumSteps();
                blockProcess.steps += processSpectrumSteps;

                blockProcess.steps += stft.synthesiseSteps() + 1;
            }
            
            size_t processToStep = newBlock ? blockProcess.steps : 0;
            
            while (blockProcess.step < processToStep) {
                size_t step = blockProcess.step++;
                if (blockProcess.newSpectrum) {
                    if (blockProcess.reanalysePrev) {
                        // analyse past input
                        if (step < stft.analyseSteps()) {
                            stashedInput.swap(stft.input);
                            stft.analyseStep(step, stft.defaultInterval());
                            stashedInput.swap(stft.input);
                            continue;
                        }
                        step -= stft.analyseSteps();
                        if (step < 1) {
                            // Copy previous analysis to our band objects
                            for (int c = 0; c < channels; ++c) {
                                auto channelBands = bandsForChannel(c);
                                auto *spectrumBands = stft.spectrum(c);
                                for (int b = 0; b < bands; ++b) {
                                    channelBands[b].prevInput = spectrumBands[b];
                                }
                            }
                            continue;
                        }
                        step -= 1;
                    }

                    // Analyse latest (stashed) input
                    if (step < stft.analyseSteps()) {
                        stashedInput.swap(stft.input);
                        stft.analyseStep(step);
                        stashedInput.swap(stft.input);
                        continue;
                    }
                    step -= stft.analyseSteps();
                    if (step < 1) {
                        // Copy analysed spectrum into our band objects
                        for (int c = 0; c < channels; ++c) {
                            auto channelBands = bandsForChannel(c);
                            auto *spectrumBands = stft.spectrum(c);
                            for (int b = 0; b < bands; ++b) {
                                channelBands[b].input = spectrumBands[b];
                            }
                        }
                        continue;
                    }
                    step -= 1;
                }

                if (step < processSpectrumSteps) {
                    processSpectrum(step);
                    continue;
                }
                step -= processSpectrumSteps;

                if (step < 1) {
                    // Copy band objects into spectrum
                    for (int c = 0; c < channels; ++c) {
                        auto channelBands = bandsForChannel(c);
                        auto *spectrumBands = stft.spectrum(c);
                        for (int b = 0; b < bands; ++b) {
                            spectrumBands[b] = channelBands[b].output;
                        }
                    }
                    continue;
                }
                step -= 1;
                
                if (step < stft.synthesiseSteps()) {
                    stft.synthesiseStep(step);
                    continue;
                }
            }

            ++blockProcess.samplesSinceLast;
            for (int c = 0; c < channels; ++c) {
                auto &&outputChannel = outputs[c];
                Sample v = 0;
                stft.readOutput(c, 1, &v);
                outputChannel[outputIndex] = v;
            }
            stft.moveOutput(1);
        }
        
        copyInput(inputSamples);
        prevInputOffset -= inputSamples;
    }

    // Read the remaining output, providing no further input.  If `outputSamples` is more than one interval, it will compute additional blocks assuming a zero-valued input
    template<class Outputs>
    void flush(Outputs &&outputs, int outputSamples, Sample playbackRate=0) {
        struct Zeros {
            struct Channel {
                Sample operator[](int) {
                    return 0;
                }
            };
            Channel operator[](int) {
                return {};
            }
        } zeros;
        // If we're asked for more than an interval of extra output, then zero-pad the input
        int outputBlock = std::max<int>(0, outputSamples - stft.defaultInterval());
        if (outputBlock > 0) process(zeros, outputBlock*playbackRate, outputs, outputBlock);

        int tailSamples = outputSamples - outputBlock; // at most one interval
        tmpProcessBuffer.resize(tailSamples);
        stft.finishOutput(1);
        for (int c = 0; c < channels; ++c) {
            stft.readOutput(c, tailSamples, tmpProcessBuffer.data());
            auto &&outputChannel = outputs[c];
            for (int i = 0; i < tailSamples; ++i) {
                outputChannel[outputBlock + i] = tmpProcessBuffer[i];
            }
            stft.readOutput(c, tailSamples, tailSamples, tmpProcessBuffer.data());
            for (int i = 0; i < tailSamples; ++i) {
                outputChannel[outputBlock + tailSamples - 1 - i] -= tmpProcessBuffer[i];
            }
        }
        stft.reset(0.1f);
        // Reset the phase-vocoder stuff, so the next block gets a fresh start
        for (int c = 0; c < channels; ++c) {
            auto channelBands = bandsForChannel(c);
            for (int b = 0; b < bands; ++b) {
                channelBands[b].prevInput = channelBands[b].output = 0;
            }
        }
    }

private:
    struct {
        size_t samplesSinceLast = std::numeric_limits<size_t>::max();
        size_t steps = 0;
        size_t step = 0;
        
        bool newSpectrum = false;
        bool reanalysePrev = false;
        bool mappedFrequencies = false;
        bool processFormants = false;
        Sample timeFactor;
    } blockProcess;

    using Complex = std::complex<Sample>;
    static constexpr Sample noiseFloor{Sample(1e-15)};
    static constexpr Sample maxCleanStretch{2}; // time-stretch ratio before we start randomising phases
    size_t silenceCounter = 0;
    bool silenceFirst = true;

    Sample freqMultiplier = 1, freqTonalityLimit = 0.5;
    
    bool formantCompensation = false; // compensate for pitch/freq change
    Sample formantMultiplier = 1, invFormantMultiplier = 1;

    using STFT = Stft<Sample>;
    STFT stft;
    typename STFT::Input stashedInput;
    
    std::vector<Sample> tmpProcessBuffer;

    int channels = 0, bands = 0;
    int prevInputOffset = -1;

    Sample bandToFreq(Sample b) const {
        return stft.binToFreq(b);
    }
    Sample freqToBand(Sample f) const {
        return stft.freqToBin(f);
    }
    
    struct Band {
        Complex input, prevInput{0};
        Complex output{0};
        Sample inputEnergy;
    };
    std::vector<Band> _channelBands;
    Band * bandsForChannel(int channel) {
        return _channelBands.data() + channel*bands;
    }
    template<Complex Band::*member>
    Complex getBand(int channel, int index) {
        if (index < 0 || index >= bands) return 0;
        return _channelBands[index + channel*bands].*member;
    }
    template<Complex Band::*member>
    Complex getFractional(int channel, int lowIndex, Sample fractional) {
        Complex low = getBand<member>(channel, lowIndex);
        Complex high = getBand<member>(channel, lowIndex + 1);
        return low + (high - low)*fractional;
    }
    template<Complex Band::*member>
    Complex getFractional(int channel, Sample inputIndex) {
        int lowIndex = static_cast<int>(std::floor(inputIndex));
        Sample fracIndex = inputIndex - lowIndex;
        return getFractional<member>(channel, lowIndex, fracIndex);
    }
    template<Sample Band::*member>
    Sample getBand(int channel, int index) {
        if (index < 0 || index >= bands) return 0;
        return _channelBands[index + channel*bands].*member;
    }
    template<Sample Band::*member>
    Sample getFractional(int channel, int lowIndex, Sample fractional) {
        Sample low = getBand<member>(channel, lowIndex);
        Sample high = getBand<member>(channel, lowIndex + 1);
        return low + (high - low)*fractional;
    }
    template<Sample Band::*member>
    Sample getFractional(int channel, Sample inputIndex) {
        int lowIndex = std::floor(inputIndex);
        Sample fracIndex = inputIndex - lowIndex;
        return getFractional<member>(channel, lowIndex, fracIndex);
    }

    struct Peak {
        Sample input, output;
    };
    std::vector<Peak> peaks;
    std::vector<Sample> energy, smoothedEnergy;
    struct PitchMapPoint {
        Sample inputBin, freqGrad;
    };
    std::vector<PitchMapPoint> outputMap;
    
    struct Prediction {
        Sample energy = 0;
        Complex input;

        Complex makeOutput(Complex phase) {
            Sample phaseNorm = stretcher_impl::norm(phase);
            if (phaseNorm <= noiseFloor) {
                phase = input; // prediction is too weak, fall back to the input
                phaseNorm = stretcher_impl::norm(input) + noiseFloor;
            }
            return phase*std::sqrt(energy/phaseNorm);
        }
    };
    std::vector<Prediction> channelPredictions;
    Prediction * predictionsForChannel(int c) {
        return channelPredictions.data() + c*bands;
    }

    std::default_random_engine randomEngine;

    size_t processSpectrumSteps = 0;
    static constexpr size_t splitMainPrediction = 8; // it's just heavy, since we're blending up to 4 different phase predictions
    void updateProcessSpectrumSteps() {
        processSpectrumSteps = 0;
        if (blockProcess.newSpectrum) processSpectrumSteps += channels;
        if (blockProcess.mappedFrequencies) {
            processSpectrumSteps += smoothEnergySteps;
            processSpectrumSteps += 1; // findPeaks
        }
        processSpectrumSteps += 1; // updating the output map
        processSpectrumSteps += channels; // preliminary phase-vocoder prediction
        processSpectrumSteps += splitMainPrediction;
        if (blockProcess.newSpectrum) processSpectrumSteps += 1; // .input -> .prevInput
        if (blockProcess.processFormants) processSpectrumSteps += 3;
    }
    void processSpectrum(size_t step) {
        Sample timeFactor = blockProcess.timeFactor;
        
        Sample smoothingBins = Sample(stft.fftSamples())/stft.defaultInterval();
        int longVerticalStep = static_cast<int>(std::round(smoothingBins));
        timeFactor = std::max<Sample>(timeFactor, 1/maxCleanStretch);
        bool randomTimeFactor = (timeFactor > maxCleanStretch);
        std::uniform_real_distribution<Sample> timeFactorDist(maxCleanStretch*2*randomTimeFactor - timeFactor, timeFactor);

        if (blockProcess.newSpectrum) {
            if (step < size_t(channels)) {
                int channel = int(step);
                auto bins = bandsForChannel(channel);

                Complex rot = std::polar(Sample(1), bandToFreq(0)*stft.defaultInterval()*Sample(2*M_PI));
                Sample freqStep = bandToFreq(1) - bandToFreq(0);
                Complex rotStep = std::polar(Sample(1), freqStep*stft.defaultInterval()*Sample(2*M_PI));
                 
                for (int b = 0; b < bands; ++b) {
                    auto &bin = bins[b];
                    bin.output = stretcher_impl::mul(bin.output, rot);
                    bin.prevInput = stretcher_impl::mul(bin.prevInput, rot);
                    rot = stretcher_impl::mul(rot, rotStep);
                }
                return;
            }
            step -= channels;
        }
        if (blockProcess.mappedFrequencies) {
            if (step < smoothEnergySteps) {
                smoothEnergy(step, smoothingBins);
                return;
            }
            step -= smoothEnergySteps;
            if (step-- == 0) {
                findPeaks();
                return;
            }
        }
        if (step-- == 0) {
            if (blockProcess.mappedFrequencies) {
                updateOutputMap();
            } else { // we're not pitch-shifting, so no need to find peaks etc.
                for (int c = 0; c < channels; ++c) {
                    Band *bins = bandsForChannel(c);
                    for (int b = 0; b < bands; ++b) {
                        bins[b].inputEnergy = stretcher_impl::norm(bins[b].input);
                    }
                }

                for (int b = 0; b < bands; ++b) {
                    outputMap[b] = {Sample(b), 1};
                }
            }
            return;
        }
        if (blockProcess.processFormants) {
            if (step < 3) {
                updateFormants(step);
                return;
            }
            step -= 3;
        }
        // Preliminary output prediction from phase-vocoder
        if (step < size_t(channels)) {
            int c = int(step);
            Band *bins = bandsForChannel(c);
            auto *predictions = predictionsForChannel(c);
            for (int b = 0; b < bands; ++b) {
                auto mapPoint = outputMap[b];
                int lowIndex = static_cast<int>(std::floor(mapPoint.inputBin));
                Sample fracIndex = mapPoint.inputBin - lowIndex;

                Prediction &prediction = predictions[b];
                Sample prevEnergy = prediction.energy;
                prediction.energy = getFractional<&Band::inputEnergy>(c, lowIndex, fracIndex);
                prediction.energy *= std::max<Sample>(0, mapPoint.freqGrad); // scale the energy according to local stretch factor
                prediction.input = getFractional<&Band::input>(c, lowIndex, fracIndex);

                auto &outputBin = bins[b];
                Complex prevInput = getFractional<&Band::prevInput>(c, lowIndex, fracIndex);
                Complex freqTwist = stretcher_impl::mul<true>(prediction.input, prevInput);
                Complex phase = stretcher_impl::mul(outputBin.output, freqTwist);
                outputBin.output = phase/(std::max(prevEnergy, prediction.energy) + noiseFloor);
            }
            return;
        }
        step -= channels;

        if (step < splitMainPrediction) {
            // Re-predict using phase differences between frequencies
            size_t chunk = step;
            int startB = int(bands*chunk/splitMainPrediction);
            int endB = int(bands*(chunk + 1)/splitMainPrediction);
            for (int b = startB; b < endB; ++b) {
                // Find maximum-energy channel and calculate that
                int maxChannel = 0;
                Sample maxEnergy = predictionsForChannel(0)[b].energy;
                for (int c = 1; c < channels; ++c) {
                    Sample e = predictionsForChannel(c)[b].energy;
                    if (e > maxEnergy) {
                        maxChannel = c;
                        maxEnergy = e;
                    }
                }

                auto *predictions = predictionsForChannel(maxChannel);
                auto &prediction = predictions[b];
                auto *bins = bandsForChannel(maxChannel);
                auto &outputBin = bins[b];

                Complex phase = 0;
                auto mapPoint = outputMap[b];

                // Upwards vertical steps
                if (b > 0) {
                    Sample binTimeFactor = randomTimeFactor ? timeFactorDist(randomEngine) : timeFactor;
                    Complex downInput = getFractional<&Band::input>(maxChannel, mapPoint.inputBin - binTimeFactor);
                    Complex shortVerticalTwist = stretcher_impl::mul<true>(prediction.input, downInput);

                    auto &downBin = bins[b - 1];
                    phase += stretcher_impl::mul(downBin.output, shortVerticalTwist);
                    
                    if (b >= longVerticalStep) {
                        Complex longDownInput = getFractional<&Band::input>(maxChannel, mapPoint.inputBin - longVerticalStep*binTimeFactor);
                        Complex longVerticalTwist = stretcher_impl::mul<true>(prediction.input, longDownInput);

                        auto &longDownBin = bins[b - longVerticalStep];
                        phase += stretcher_impl::mul(longDownBin.output, longVerticalTwist);
                    }
                }
                // Downwards vertical steps
                if (b < bands - 1) {
                    auto &upPrediction = predictions[b + 1];
                    auto &upMapPoint = outputMap[b + 1];

                    Sample binTimeFactor = randomTimeFactor ? timeFactorDist(randomEngine) : timeFactor;
                    Complex downInput = getFractional<&Band::input>(maxChannel, upMapPoint.inputBin - binTimeFactor);
                    Complex shortVerticalTwist = stretcher_impl::mul<true>(upPrediction.input, downInput);

                    auto &upBin = bins[b + 1];
                    phase += stretcher_impl::mul<true>(upBin.output, shortVerticalTwist);
                    
                    if (b < bands - longVerticalStep) {
                        auto &longUpPrediction = predictions[b + longVerticalStep];
                        auto &longUpMapPoint = outputMap[b + longVerticalStep];

                        Complex longDownInput = getFractional<&Band::input>(maxChannel, longUpMapPoint.inputBin - longVerticalStep*binTimeFactor);
                        Complex longVerticalTwist = stretcher_impl::mul<true>(longUpPrediction.input, longDownInput);

                        auto &longUpBin = bins[b + longVerticalStep];
                        phase += stretcher_impl::mul<true>(longUpBin.output, longVerticalTwist);
                    }
                }

                outputBin.output = prediction.makeOutput(phase);
                
                // All other bins are locked in phase
                for (int c = 0; c < channels; ++c) {
                    if (c != maxChannel) {
                        auto &channelBin = bandsForChannel(c)[b];
                        auto &channelPrediction = predictionsForChannel(c)[b];
                        
                        Complex channelTwist = stretcher_impl::mul<true>(channelPrediction.input, prediction.input);
                        Complex channelPhase = stretcher_impl::mul(outputBin.output, channelTwist);
                        channelBin.output = channelPrediction.makeOutput(channelPhase);
                    }
                }
            }
            return;
        }
        step -= splitMainPrediction;

        if (blockProcess.newSpectrum) {
            if (step-- == 0) {
                for (auto &bin : _channelBands) {
                    bin.prevInput = bin.input;
                }
            }
        }
    }
    
    // Produces smoothed energy across all channels
    static constexpr size_t smoothEnergySteps = 3;
    Sample smoothEnergyState = 0;
    void smoothEnergy(size_t step, Sample smoothingBins) {
        Sample smoothingSlew = 1/(1 + smoothingBins*Sample(0.5));
        if (step-- == 0) {
            for (auto &e : energy) e = 0;
            for (int c = 0; c < channels; ++c) {
                Band *bins = bandsForChannel(c);
                for (int b = 0; b < bands; ++b) {
                    Sample e = stretcher_impl::norm(bins[b].input);
                    bins[b].inputEnergy = e; // Used for interpolating prediction energy
                    energy[b] += e;
                }
            }
            for (int b = 0; b < bands; ++b) {
                smoothedEnergy[b] = energy[b];
            }
            smoothEnergyState = 0;
            return;
        }

        // The two other steps are repeated smoothing passes, down and up
        Sample e = smoothEnergyState;
        for (int b = bands - 1; b >= 0; --b) {
            e += (smoothedEnergy[b] - e)*smoothingSlew;
            smoothedEnergy[b] = e;
        }
        for (int b = 0; b < bands; ++b) {
            e += (smoothedEnergy[b] - e)*smoothingSlew;
            smoothedEnergy[b] = e;
        }
        smoothEnergyState = e;
    }
    
    Sample mapFreq(Sample freq) const {
        if (freq > freqTonalityLimit) {
            return freq + (freqMultiplier - 1)*freqTonalityLimit;
        }
        return freq*freqMultiplier;
    }
    
    // Identifies spectral peaks using energy across all channels
    void findPeaks() {
        peaks.resize(0);
        
        int start = 0;
        while (start < bands) {
            if (energy[start] > smoothedEnergy[start]) {
                int end = start;
                Sample bandSum = 0, energySum = 0;
                while (end < bands && energy[end] > smoothedEnergy[end]) {
                    bandSum += end*energy[end];
                    energySum += energy[end];
                    ++end;
                }
                Sample avgBand = bandSum/energySum;
                Sample avgFreq = bandToFreq(avgBand);
                peaks.emplace_back(Peak{avgBand, freqToBand(mapFreq(avgFreq))});

                start = end;
            }
            ++start;
        }
    }
    
    void updateOutputMap() {
        if (peaks.empty()) {
            for (int b = 0; b < bands; ++b) {
                outputMap[b] = {Sample(b), 1};
            }
            return;
        }
        Sample bottomOffset = peaks[0].input - peaks[0].output;
        for (int b = 0; b < std::min(bands, static_cast<int>(std::ceil(peaks[0].output))); ++b) {
            outputMap[b] = {b + bottomOffset, 1};
        }
        // Interpolate between points
        for (size_t p = 1; p < peaks.size(); ++p) {
            const Peak &prev = peaks[p - 1], &next = peaks[p];
            Sample rangeScale = 1/(next.output - prev.output);
            Sample outOffset = prev.input - prev.output;
            Sample outScale = next.input - next.output - prev.input + prev.output;
            Sample gradScale = outScale*rangeScale;
            int startBin = std::max(0, static_cast<int>(std::ceil(prev.output)));
            int endBin = std::min(bands, static_cast<int>(std::ceil(next.output)));
            for (int b = startBin; b < endBin; ++b) {
                Sample r = (b - prev.output)*rangeScale;
                Sample h = r*r*(3 - 2*r);
                Sample outB = b + outOffset + h*outScale;
                
                Sample gradH = 6*r*(1 - r);
                Sample gradB = 1 + gradH*gradScale;
                
                outputMap[b] = {outB, gradB};
            }
        }
        Sample topOffset = peaks.back().input - peaks.back().output;
        for (int b = std::max(0, static_cast<int>(peaks.back().output)); b < bands; ++b) {
            outputMap[b] = {b + topOffset, 1};
        }
    }

    // If we mapped formants the same way as mapFreq(), this would be the inverse
    Sample invMapFormant(Sample freq) const {
        if (freq*invFormantMultiplier > freqTonalityLimit) {
            return freq + (1 - formantMultiplier)*freqTonalityLimit;
        }
        return freq*invFormantMultiplier;
    }

    Sample freqEstimateWeighted = 0;
    Sample freqEstimateWeight = 0;
    Sample estimateFrequency() {
        // 3 highest peaks in the input
        std::array<int, 3> peakIndices{0, 0, 0};
        for (int b = 1; b < bands - 1; ++b) {
            Sample e = formantMetric[b];
            // local maxima only
            if (e < formantMetric[b - 1] || e <= formantMetric[b + 1]) continue;
            
            if (e > formantMetric[peakIndices[0]]) {
                if (e > formantMetric[peakIndices[1]]) {
                    if (e > formantMetric[peakIndices[2]]) {
                        peakIndices = {peakIndices[1], peakIndices[2], b};
                    } else {
                        peakIndices = {peakIndices[1], b, peakIndices[2]};
                    }
                } else {
                    peakIndices[0] = b;
                }
            }
        }
        
        // VERY rough pitch estimation
        int peakEstimate = peakIndices[2];
        if (formantMetric[peakIndices[1]] > formantMetric[peakIndices[2]]*0.1) {
            int diff = std::abs(peakEstimate - peakIndices[1]);
            if (diff > peakEstimate/8 && diff < peakEstimate*7/8) peakEstimate = peakEstimate%diff;
            if (formantMetric[peakIndices[0]] > formantMetric[peakIndices[2]]*0.01) {
                diff = std::abs(peakEstimate - peakIndices[0]);
                if (diff > peakEstimate/8 && diff < peakEstimate*7/8) peakEstimate = peakEstimate%diff;
            }
        }
        Sample weight = formantMetric[peakIndices[2]];
        // Smooth it out a bit
        freqEstimateWeighted += (peakEstimate*weight - freqEstimateWeighted)*Sample(0.25);
        freqEstimateWeight += (weight - freqEstimateWeight)*Sample(0.25);
        
        return freqEstimateWeighted/(freqEstimateWeight + Sample(1e-30));
    }
    
    Sample freqEstimate;
    
    std::vector<Sample> formantMetric;
    Sample formantBaseFreq = 0;
    void updateFormants(size_t step) {
        if (step-- == 0) {
            for (auto &e : formantMetric) e = 0;
            for (int c = 0; c < channels; ++c) {
                Band *bins = bandsForChannel(c);
                for (int b = 0; b < bands; ++b) {
                    formantMetric[b] += bins[b].inputEnergy;
                }
            }

            freqEstimate = freqToBand(formantBaseFreq);
            if (formantBaseFreq <= 0) freqEstimate = estimateFrequency();
        } else if (step-- == 0) {
            Sample decay = 1 - 1/(freqEstimate*Sample(0.5) + 1);
            Sample e = 0;
            for (size_t repeat = 0; repeat < 2; ++repeat) {
                for (int b = bands - 1; b >= 0; --b) {
                    e = std::max(formantMetric[b], e*decay);
                    formantMetric[b] = e;
                }
                for (int b = 0; b < bands; ++b) {
                    e = std::max(formantMetric[b], e*decay);
                    formantMetric[b] = e;
                }
            }
            decay = 1/decay;
            for (size_t repeat = 0; repeat < 2; ++repeat) {
                for (int b = bands - 1; b >= 0; --b) {
                    e = std::min(formantMetric[b], e*decay);
                    formantMetric[b] = e;
                }
                for (int b = 0; b < bands; ++b) {
                    e = std::min(formantMetric[b], e*decay);
                    formantMetric[b] = e;
                }
            }
        } else {
            auto getFormant = [&](Sample band) -> Sample {
                if (band < 0) return 0;
                band = std::min(band, static_cast<Sample>(bands));
                int floorBand = std::floor(band);
                Sample fracBand = band - floorBand;
                Sample low = formantMetric[floorBand], high = formantMetric[floorBand + 1];
                return low + (high - low)*fracBand;
            };

            for (int b = 0; b < bands; ++b) {
                Sample inputF = bandToFreq(static_cast<Sample>(b));
                Sample outputF = formantCompensation ? mapFreq(inputF) : inputF;
                outputF = invMapFormant(outputF);

                Sample inputE = formantMetric[b];
                Sample targetE = getFormant(freqToBand(outputF));

                Sample formantRatio = targetE/(inputE + Sample(1e-30));
                Sample energyRatio = formantRatio;

                for (int c = 0; c < channels; ++c) {
                    Band *bins = bandsForChannel(c);
                    // This is what's used to decide the output energy, so this affects the output
                    bins[b].inputEnergy *= energyRatio;
                }
            }
        }
    }

};

}} // namespace nvc::spectral

#endif
