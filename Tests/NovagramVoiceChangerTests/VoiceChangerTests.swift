// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import NovagramVoiceChanger
import NovagramVoiceChangerCore
import XCTest

final class VoiceChangerTests: XCTestCase {
    // MARK: - Off

    func testOffIsBitIdenticalPassthrough() {
        var generator = SystemRandomNumberGenerator()
        let input = (0..<48000).map { _ in Int16.random(in: Int16.min...Int16.max, using: &generator) }
        XCTAssertTrue(TestSignals.renderMessage(input, preset: .off, sampleRate: 48000, chunk: 1777) == input)
        XCTAssertTrue(TestSignals.renderCall(input, preset: .off, sampleRate: 48000) == input)
        // Modulation at 0 semitones is also a passthrough in message mode.
        XCTAssertTrue(TestSignals.renderMessage(input, preset: .modulation, params: VoiceChangerParams(tone: 0), sampleRate: 48000) == input)
    }

    // MARK: - Pitch-aware presets

    func testWomanLiftsA120HzVoiceTo210Hz() {
        let input = TestSignals.sawtooth(hz: 120, seconds: 3, sampleRate: 48000)
        let output = TestSignals.renderMessage(input, preset: .woman, sampleRate: 48000)
        XCTAssertEqual(output.count, input.count)
        let f0 = TestSignals.fundamental(output, sampleRate: 48000, from: 1.0, to: 2.5)
        print("[nvc] woman, 120 Hz in -> \(String(format: "%.1f", f0)) Hz out (message mode)")
        XCTAssertEqual(f0, 210, accuracy: 21)
    }

    func testWomanKeepsA220HzVoiceNear210Hz() {
        // Android's fixed +9 semitones would give 370 Hz here; the pitch-aware preset aims at 210 Hz.
        let input = TestSignals.sawtooth(hz: 220, seconds: 3, sampleRate: 48000)
        let output = TestSignals.renderMessage(input, preset: .woman, sampleRate: 48000)
        let f0 = TestSignals.fundamental(output, sampleRate: 48000, from: 0.2, to: 2.5)
        print("[nvc] woman, 220 Hz in -> \(String(format: "%.1f", f0)) Hz out (message mode, from 0.2 s)")
        XCTAssertEqual(f0, 210, accuracy: 21)
        let fixed = TestSignals.renderMessage(input, preset: .woman, params: VoiceChangerParams(pitchAware: false), sampleRate: 48000)
        let fixedF0 = TestSignals.fundamental(fixed, sampleRate: 48000, from: 0.2, to: 2.5)
        print("[nvc] woman (fixed +9 st, Android), 220 Hz in -> \(String(format: "%.1f", fixedF0)) Hz out")
        XCTAssertEqual(fixedF0, 220 * pow(2, 9.0 / 12), accuracy: 20)
    }

    func testWomanAdaptsInCallMode() {
        for (rate, hz) in [(16000, 220.0), (48000, 120.0)] {
            let input = TestSignals.sawtooth(hz: hz, seconds: 4, sampleRate: rate)
            let output = TestSignals.renderCall(input, preset: .woman, sampleRate: rate)
            let f0 = TestSignals.fundamental(output, sampleRate: rate, from: 2.5, to: 4)
            print("[nvc] woman, call mode \(rate) Hz, \(Int(hz)) Hz in -> \(String(format: "%.1f", f0)) Hz out")
            XCTAssertEqual(f0, 210, accuracy: 21)
        }
    }

    func testHumanVoicePresetsReachTheirTargetsFromLowAndHighVoices() {
        let targets: [(VoiceChangerPreset, Double)] = [(.man, 110), (.woman, 210), (.child, 270), (.littleKid, 330), (.youngGirl, 240)]
        for (preset, target) in targets {
            for inputHz in [120.0, 220.0] {
                let input = TestSignals.sawtooth(hz: inputHz, seconds: 3, sampleRate: 48000)
                let output = TestSignals.renderMessage(input, preset: preset, sampleRate: 48000)
                let f0 = TestSignals.fundamental(output, sampleRate: 48000, from: 0.5, to: 2.5)
                print("[nvc] \(preset), \(Int(inputHz)) Hz in -> \(String(format: "%.1f", f0)) Hz out (target \(Int(target)))")
                XCTAssertEqual(f0, target, accuracy: target * 0.1, "\(preset) from \(inputHz) Hz")
            }
        }
        // The owner's case: a 125 Hz man stays a natural 110 Hz man (Android's fixed -6 st gave 88 Hz).
        let man = TestSignals.renderMessage(TestSignals.sawtooth(hz: 125, seconds: 3, sampleRate: 48000), preset: .man, sampleRate: 48000)
        let manF0 = TestSignals.fundamental(man, sampleRate: 48000, from: 0.5, to: 2.5)
        print("[nvc] man, 125 Hz in -> \(String(format: "%.1f", manF0)) Hz out")
        XCTAssertEqual(manF0, 110, accuracy: 5)
        let monster = TestSignals.renderMessage(TestSignals.sawtooth(hz: 120, seconds: 3, sampleRate: 48000), preset: .monster, sampleRate: 48000)
        let monsterF0 = TestSignals.fundamental(monster, sampleRate: 48000, from: 0.5, to: 2.5)
        print("[nvc] monster, 120 Hz in -> \(String(format: "%.1f", monsterF0)) Hz out (target 65)")
        XCTAssertEqual(monsterF0, 65, accuracy: 6.5)
    }

    func testSpeakerPitchIsTracked() {
        guard let changer = VoiceChanger(mode: .call, sampleRate: 48000) else {
            return XCTFail("no engine")
        }
        _ = TestSignals.renderCall(TestSignals.sawtooth(hz: 150, seconds: 1, sampleRate: 48000), preset: .off, sampleRate: 48000, changer: changer)
        let pitch = changer.speakerPitchHz ?? 0
        print("[nvc] speaker pitch of a 150 Hz sawtooth: \(String(format: "%.2f", pitch)) Hz")
        XCTAssertEqual(Double(pitch), 150, accuracy: 3)

        guard let noiseChanger = VoiceChanger(mode: .call, sampleRate: 48000) else {
            return XCTFail("no engine")
        }
        var generator = SystemRandomNumberGenerator()
        let noise = (0..<48000).map { _ in Int16.random(in: -8000...8000, using: &generator) }
        _ = TestSignals.renderCall(noise, preset: .off, sampleRate: 48000, changer: noiseChanger)
        XCTAssertNil(noiseChanger.speakerPitchHz, "white noise is unvoiced")
    }

    // MARK: - Auto-tune

    func testAutoTuneSnapsToTheNearestSemitone() {
        for (inputHz, expected) in [(452.0, 440.0), (229.0, 233.08)] {
            let input = TestSignals.sawtooth(hz: inputHz, seconds: 2, sampleRate: 48000)
            let output = TestSignals.renderMessage(input, preset: .autoTune, params: VoiceChangerParams(retuneSpeed: 0), sampleRate: 48000)
            let f0 = TestSignals.fundamental(output, sampleRate: 48000, from: 0.5, to: 1.8)
            print("[nvc] auto-tune, \(inputHz) Hz in -> \(String(format: "%.1f", f0)) Hz out (nearest note \(expected))")
            XCTAssertEqual(f0, expected, accuracy: expected * 0.01)
        }
    }

    // MARK: - Tape speed

    func testTapeSpeedChangesTheLength() {
        let input = TestSignals.sawtooth(hz: 150, seconds: 2, sampleRate: 48000)
        for (preset, rate) in [(VoiceChangerPreset.speed1_50, 1.5), (.speed0_70, 0.7)] {
            let output = TestSignals.renderMessage(input, preset: preset, sampleRate: 48000)
            let expected = Double(input.count) / rate
            print("[nvc] tape \(rate): \(input.count) -> \(output.count) samples (expected \(Int(expected)))")
            XCTAssertEqual(Double(output.count), expected, accuracy: expected * 0.01)
            let f0 = TestSignals.fundamental(output, sampleRate: 48000, from: 0.2, to: 1.2)
            XCTAssertEqual(f0, 150 * rate, accuracy: 150 * rate * 0.02)
        }
        // A call never changes its length: a tape speed counts as Off there.
        XCTAssertTrue(TestSignals.renderCall(input, preset: .speed1_50, sampleRate: 48000) == input)
    }

    // MARK: - Call mode

    func testCallModeKeepsBlocksAndLatency() {
        for rate in [48000, 16000] {
            guard let changer = VoiceChanger(mode: .call, sampleRate: rate) else {
                return XCTFail("no engine")
            }
            changer.setPreset(.woman)
            let block = rate / 100
            let silence = [Int16](repeating: 0, count: rate * 3 / 10)
            let input = silence + TestSignals.sawtooth(hz: 150, seconds: 1, sampleRate: rate, peak: 16000)
            var output = input
            var allBlocksKept = true
            output.withUnsafeMutableBufferPointer { buffer in
                var position = 0
                while position + block <= buffer.count {
                    let slice = UnsafeMutableBufferPointer(rebasing: buffer[position..<(position + block)])
                    if nvcCallProcess(changer, slice) != Int32(block) {
                        allBlocksKept = false
                    }
                    position += block
                }
            }
            XCTAssertTrue(allBlocksKept)
            XCTAssertEqual(output.count, input.count)
            let reportedMs = Double(changer.latencySamples) * 1000 / Double(rate)
            let measuredMs = (onset(output, sampleRate: rate) - onset(input, sampleRate: rate)) * 1000
            print("[nvc] call \(rate) Hz: latency reported \(String(format: "%.1f", reportedMs)) ms, measured \(String(format: "%.1f", measuredMs)) ms")
            XCTAssertLessThanOrEqual(reportedMs, 60)
            XCTAssertLessThanOrEqual(measuredMs, 60)
            XCTAssertGreaterThan(measuredMs, 0)
        }
    }

    func testPresetSwitchMidCallHasNoClicks() {
        let rate = 48000
        let input = TestSignals.sine(hz: 220, seconds: 3.2, sampleRate: rate, peak: 10000)
        var switches: [Int: VoiceChangerPreset] = [:]
        let order: [VoiceChangerPreset] = [.man, .off, .child, .monster, .autoTune, .off, .mouse, .helium, .woman]
        for (index, preset) in order.enumerated() {
            switches[30 * (index + 1)] = preset
        }
        let output = TestSignals.renderCall(input, preset: .woman, sampleRate: rate, switches: switches)
        // Skip the first 0.1 s (fade-in from silence at the start of the call).
        var maxStep = 0
        for i in (rate / 10 + 1)..<output.count {
            maxStep = max(maxStep, abs(Int(output[i]) - Int(output[i - 1])))
        }
        // A sine of 10000 at 220 Hz steps by at most ~290 per sample; even shifted 16 semitones up, ~740. A click
        // (a jump between unrelated samples) would be thousands.
        print("[nvc] preset switches every 0.3 s: largest sample step \(maxStep) (input peak 10000)")
        XCTAssertLessThan(maxStep, 2000)
    }

    // MARK: - Level

    func testLoudInputNeverReachesFullScale() {
        let loud = TestSignals.sawtooth(hz: 140, seconds: 1.5, sampleRate: 48000, peak: 32767)
        let square = TestSignals.sine(hz: 180, seconds: 1.5, sampleRate: 48000, peak: 90000)
        for preset in [VoiceChangerPreset.woman, .man, .child, .littleKid, .youngGirl, .mouse, .monster, .helium, .hexafluoride, .alien, .autoTune, .speed1_50, .robot] {
            for input in [loud, square] {
                let message = TestSignals.renderMessage(input, preset: preset, sampleRate: 48000)
                // A tape speed is Off in a call, a bit-identical passthrough, so only its message counts.
                let call = preset.isTapeSpeed ? [] : TestSignals.renderCall(input, preset: preset, sampleRate: 48000)
                let peak = max(message.map { abs(Int($0)) }.max() ?? 0, call.map { abs(Int($0)) }.max() ?? 0)
                XCTAssertLessThan(peak, 32767, "\(preset) peaked at \(peak)")
            }
        }
    }

    // MARK: - Stage chain seam

    func testStageChainSeamIsCalledAndReleased() {
        final class Counter: @unchecked Sendable {
            var processed = 0
            var released = 0
        }
        let counter = Counter()
        let context = Unmanaged.passRetained(counter).toOpaque()
        let chain = nvc_stage_chain(
            context: context,
            process: { context, samples, count in
                let counter = Unmanaged<Counter>.fromOpaque(context!).takeUnretainedValue()
                counter.processed += Int(count)
                for i in 0..<Int(count) {
                    samples![i] *= 0.5
                }
            },
            release: { context in
                Unmanaged<Counter>.fromOpaque(context!).takeUnretainedValue().released += 1
            },
            latency_samples: 0,
            tail_samples: 0
        )
        do {
            guard let changer = VoiceChanger(mode: .call, sampleRate: 16000) else {
                return XCTFail("no engine")
            }
            XCTAssertTrue(changer.setPreset(.robot, stageChain: chain))
            var block = TestSignals.sine(hz: 200, seconds: 0.5, sampleRate: 16000, peak: 8000)
            _ = TestSignals.renderCall(block, preset: nil, sampleRate: 16000, changer: changer)
            XCTAssertGreaterThan(counter.processed, 0)
            XCTAssertEqual(counter.released, 0)
            changer.setPreset(.off) // the chain retires after the 20 ms fade-out...
            block = Array(block[0..<160])
            for _ in 0..<4 {
                XCTAssertTrue(changer.processCall(&block))
            }
            changer.setPreset(.off) // ...and is released on the next control call
            XCTAssertEqual(counter.released, 1)
        }
        XCTAssertEqual(counter.released, 1, "released exactly once")
        Unmanaged<Counter>.fromOpaque(context).release()
    }

    // MARK: - Speed

    func testCallModeRealtimeFactor() {
        let rate = 48000
        let input = TestSignals.sawtooth(hz: 130, seconds: 10, sampleRate: rate)
        guard let changer = VoiceChanger(mode: .call, sampleRate: rate) else {
            return XCTFail("no engine")
        }
        let start = Date()
        _ = TestSignals.renderCall(input, preset: .woman, sampleRate: rate, changer: changer)
        let factor = Date().timeIntervalSince(start) / 10
        #if DEBUG
        let build = "debug (-O0)"
        #else
        let build = "release"
        #endif
        print("[nvc] call mode 48 kHz woman: realtime factor \(String(format: "%.4f", factor)) (\(build))")
        #if !DEBUG
        XCTAssertLessThan(factor, 0.1)
        #endif
    }

    // MARK: - Helpers

    private func nvcCallProcess(_ changer: VoiceChanger, _ slice: UnsafeMutableBufferPointer<Int16>) -> Int32 {
        return changer.processCall(slice) ? Int32(slice.count) : -1
    }

    /// When the 2 ms envelope first reaches half of its later steady level, in seconds.
    private func onset(_ samples: [Int16], sampleRate: Int) -> Double {
        let window = sampleRate / 500
        var envelope = [Double](repeating: 0, count: samples.count)
        var sum = 0.0
        for i in 0..<samples.count {
            sum += abs(Double(samples[i]))
            if i >= window {
                sum -= abs(Double(samples[i - window]))
            }
            envelope[i] = sum / Double(window)
        }
        let steady = envelope[(samples.count * 3 / 4)...].reduce(0, +) / Double(samples.count - samples.count * 3 / 4)
        let index = envelope.firstIndex { $0 >= steady / 2 } ?? 0
        return Double(index) / Double(sampleRate)
    }
}
