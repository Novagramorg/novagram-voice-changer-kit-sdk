// SPDX-License-Identifier: GPL-2.0-or-later
import Accelerate
import Foundation
import NovagramVoiceChanger

/// Test signals and measurements, independent of the engine's own pitch tracker.
enum TestSignals {
    /// A band-limited sawtooth (harmonics up to 5 kHz, amplitude 1/k), scaled to `peak` (in 16-bit units).
    static func sawtooth(hz: Double, seconds: Double, sampleRate: Int, peak: Double = 12000) -> [Int16] {
        let count = Int(seconds * Double(sampleRate))
        let harmonics = max(1, Int(min(5000, Double(sampleRate) * 0.45) / hz))
        var values = [Double](repeating: 0, count: count)
        for k in 1...harmonics {
            let step = 2 * Double.pi * hz * Double(k) / Double(sampleRate)
            let amplitude = 1 / Double(k)
            for i in 0..<count {
                values[i] += amplitude * sin(step * Double(i))
            }
        }
        let maxValue = values.map { abs($0) }.max() ?? 1
        return values.map { Int16(clamping: Int(($0 / maxValue * peak).rounded())) }
    }

    static func sine(hz: Double, seconds: Double, sampleRate: Int, peak: Double) -> [Int16] {
        let count = Int(seconds * Double(sampleRate))
        let step = 2 * Double.pi * hz / Double(sampleRate)
        return (0..<count).map { Int16(clamping: Int((sin(step * Double($0)) * peak).rounded())) }
    }

    /// The fundamental of `samples[from..<to]` (seconds) by normalised autocorrelation: the shortest lag whose peak is
    /// within 85 % of the best one (so a subharmonic never wins), refined by a parabola; the median over 85 ms windows.
    static func fundamental(_ samples: [Int16], sampleRate: Int, from: Double, to: Double) -> Double {
        let start = Int(from * Double(sampleRate))
        let end = min(samples.count, Int(to * Double(sampleRate)))
        let x = samples[start..<end].map { Float($0) }
        let minLag = sampleRate / 1000
        let maxLag = sampleRate / 50
        let window = Int(0.085 * Double(sampleRate))
        var estimates: [Double] = []
        var position = 0
        while position + window + maxLag <= x.count {
            var r = [Float](repeating: 0, count: maxLag + 2)
            x.withUnsafeBufferPointer { buffer in
                let base = buffer.baseAddress! + position
                var energy0: Float = 0
                vDSP_dotpr(base, 1, base, 1, &energy0, vDSP_Length(window))
                for lag in minLag...(maxLag + 1) {
                    var dot: Float = 0
                    var energy: Float = 0
                    vDSP_dotpr(base, 1, base + lag, 1, &dot, vDSP_Length(window))
                    vDSP_dotpr(base + lag, 1, base + lag, 1, &energy, vDSP_Length(window))
                    r[lag] = dot / max(1e-9, sqrt(energy0 * energy))
                }
            }
            let best = r[minLag...maxLag].max() ?? 0
            if best > 0.5 {
                for lag in (minLag + 1)..<maxLag where r[lag] >= 0.85 * best && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1] {
                    let a = Double(r[lag - 1]), b = Double(r[lag]), c = Double(r[lag + 1])
                    let denominator = a - 2 * b + c
                    let offset = denominator < 0 ? 0.5 * (a - c) / denominator : 0
                    estimates.append(Double(sampleRate) / (Double(lag) + offset))
                    break
                }
            }
            position += window / 2
        }
        guard !estimates.isEmpty else {
            return 0
        }
        return estimates.sorted()[estimates.count / 2]
    }

    /// The whole message: every chunk, then the drain.
    static func renderMessage(_ input: [Int16], preset: VoiceChangerPreset, params: VoiceChangerParams = .default, sampleRate: Int, chunk: Int = 4096) -> [Int16] {
        guard let changer = VoiceChanger(mode: .message, sampleRate: sampleRate) else {
            return []
        }
        changer.setPreset(preset, params: params)
        var output: [Int16] = []
        var position = 0
        while position < input.count {
            let end = min(input.count, position + chunk)
            output += changer.process(Array(input[position..<end]))
            position = end
        }
        return output + changer.drain()
    }

    /// Call mode in fixed blocks; `switches` changes the preset at the given block indices.
    static func renderCall(_ input: [Int16], preset: VoiceChangerPreset?, sampleRate: Int, switches: [Int: VoiceChangerPreset] = [:], changer existing: VoiceChanger? = nil) -> [Int16] {
        let block = sampleRate / 100
        guard let changer = existing ?? VoiceChanger(mode: .call, sampleRate: sampleRate) else {
            return []
        }
        if let preset {
            changer.setPreset(preset)
        }
        var output = input
        var index = 0
        output.withUnsafeMutableBufferPointer { buffer in
            var position = 0
            while position + block <= buffer.count {
                if let next = switches[index] {
                    changer.setPreset(next)
                }
                let slice = UnsafeMutableBufferPointer(rebasing: buffer[position..<(position + block)])
                changer.processCall(slice)
                position += block
                index += 1
            }
        }
        return output
    }
}
