// SPDX-License-Identifier: GPL-2.0-or-later
//
// Renders every preset of a WAV file for listening:
//   swift run -c release nvc-render [input.wav] [output-folder]
// Without an input it makes a short synthetic voice. Output goes to samples/ (gitignored): message-<preset>.wav for
// message mode and call-<preset>.wav for call mode (10 ms blocks, as in a call).
import Foundation
import NovagramVoiceChanger

struct Wav {
    var sampleRate: Int
    var samples: [Int16]

    /// 16-bit PCM, any channel count (mixed down to mono).
    static func read(_ url: URL) throws -> Wav {
        let data = try Data(contentsOf: url)
        func u32(_ at: Int) -> Int { Int(data[at]) | Int(data[at + 1]) << 8 | Int(data[at + 2]) << 16 | Int(data[at + 3]) << 24 }
        func u16(_ at: Int) -> Int { Int(data[at]) | Int(data[at + 1]) << 8 }
        guard data.count > 12, String(decoding: data[0..<4], as: UTF8.self) == "RIFF", String(decoding: data[8..<12], as: UTF8.self) == "WAVE" else {
            throw NSError(domain: "nvc-render", code: 1, userInfo: [NSLocalizedDescriptionKey: "not a WAV file"])
        }
        var position = 12
        var channels = 1, sampleRate = 48000, bits = 16
        while position + 8 <= data.count {
            let id = String(decoding: data[position..<(position + 4)], as: UTF8.self)
            let size = u32(position + 4)
            let body = position + 8
            if id == "fmt " {
                channels = u16(body + 2)
                sampleRate = u32(body + 4)
                bits = u16(body + 14)
            } else if id == "data" {
                guard bits == 16 else {
                    throw NSError(domain: "nvc-render", code: 2, userInfo: [NSLocalizedDescriptionKey: "only 16-bit PCM is supported"])
                }
                let frames = min(size, data.count - body) / (2 * channels)
                var samples = [Int16](repeating: 0, count: frames)
                for frame in 0..<frames {
                    var sum = 0
                    for channel in 0..<channels {
                        sum += Int(Int16(truncatingIfNeeded: u16(body + (frame * channels + channel) * 2)))
                    }
                    samples[frame] = Int16(clamping: sum / channels)
                }
                return Wav(sampleRate: sampleRate, samples: samples)
            }
            position = body + size + (size & 1)
        }
        throw NSError(domain: "nvc-render", code: 3, userInfo: [NSLocalizedDescriptionKey: "no data chunk"])
    }

    func write(_ url: URL) throws {
        var data = Data()
        func append32(_ value: Int) { withUnsafeBytes(of: UInt32(value).littleEndian) { data.append(contentsOf: $0) } }
        func append16(_ value: Int) { withUnsafeBytes(of: UInt16(value).littleEndian) { data.append(contentsOf: $0) } }
        data.append(contentsOf: Array("RIFF".utf8))
        append32(36 + samples.count * 2)
        data.append(contentsOf: Array("WAVEfmt ".utf8))
        append32(16)
        append16(1)
        append16(1)
        append32(sampleRate)
        append32(sampleRate * 2)
        append16(2)
        append16(16)
        data.append(contentsOf: Array("data".utf8))
        append32(samples.count * 2)
        for sample in samples {
            append16(Int(UInt16(bitPattern: sample)))
        }
        try data.write(to: url)
    }
}

/// A rough voice: a sawtooth gliding around 125 Hz with vibrato, through two vowel-like resonances, in syllables.
func syntheticVoice(sampleRate: Int, seconds: Double) -> [Int16] {
    let count = Int(seconds * Double(sampleRate))
    var phase = 0.0
    var output = [Int16](repeating: 0, count: count)
    var low1 = 0.0, band1 = 0.0, low2 = 0.0, band2 = 0.0
    for i in 0..<count {
        let t = Double(i) / Double(sampleRate)
        let f0 = 125 * (1 + 0.15 * sin(2 * Double.pi * 0.4 * t)) * (1 + 0.01 * sin(2 * Double.pi * 5.5 * t))
        phase += f0 / Double(sampleRate)
        phase -= floor(phase)
        let source = 2 * phase - 1
        // Two state-variable band-passes near 700 Hz and 1200 Hz.
        let g1 = 2 * sin(Double.pi * 700 / Double(sampleRate)), g2 = 2 * sin(Double.pi * 1200 / Double(sampleRate))
        low1 += g1 * band1
        band1 += g1 * (source - low1 - 0.15 * band1)
        low2 += g2 * band2
        band2 += g2 * (source - low2 - 0.2 * band2)
        let syllable = max(0, sin(2 * Double.pi * 2.2 * t))
        output[i] = Int16(clamping: Int((band1 + 0.6 * band2) * 2500 * syllable))
    }
    return output
}

/// The median pitch of a rendering, as the engine's own tracker hears it (nil for no voice).
func medianPitch(_ samples: [Int16], sampleRate: Int) -> Float? {
    guard let tracker = VoiceChanger(mode: .call, sampleRate: sampleRate) else {
        return nil
    }
    var copy = samples
    let block = sampleRate / 100
    copy.withUnsafeMutableBufferPointer { buffer in
        var position = 0
        while position + block <= buffer.count {
            tracker.processCall(UnsafeMutableBufferPointer(rebasing: buffer[position..<(position + block)]))
            position += block
        }
    }
    return tracker.speakerPitchHz
}

func hz(_ value: Float?) -> String {
    return value.map { String(format: "%.0f Hz", $0) } ?? "-"
}

let arguments = CommandLine.arguments.dropFirst()
let input: Wav
if let path = arguments.first {
    do {
        input = try Wav.read(URL(fileURLWithPath: path))
    } catch {
        FileHandle.standardError.write(Data("nvc-render: \(path): \(error.localizedDescription)\n".utf8))
        exit(1)
    }
} else {
    input = Wav(sampleRate: 48000, samples: syntheticVoice(sampleRate: 48000, seconds: 4))
}
let folder = URL(fileURLWithPath: arguments.dropFirst().first ?? "samples", isDirectory: true)
try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
try? input.write(folder.appendingPathComponent("input.wav"))

for preset in VoiceChangerPreset.allCases {
    guard let message = VoiceChanger(mode: .message, sampleRate: input.sampleRate) else {
        FileHandle.standardError.write(Data("nvc-render: unsupported sample rate \(input.sampleRate)\n".utf8))
        exit(1)
    }
    message.setPreset(preset)
    var output: [Int16] = []
    var position = 0
    while position < input.samples.count {
        let end = min(input.samples.count, position + 4096)
        output += message.process(Array(input.samples[position..<end]))
        position = end
    }
    output += message.drain()
    try? Wav(sampleRate: input.sampleRate, samples: output).write(folder.appendingPathComponent("message-\(preset).wav"))
    let name = "message-\(preset).wav".padding(toLength: 28, withPad: " ", startingAt: 0)
    print("\(name) in \(hz(medianPitch(input.samples, sampleRate: input.sampleRate))) -> out \(hz(medianPitch(output, sampleRate: input.sampleRate))), last shift \(String(format: "%+.1f", message.shiftSemitones)) st")

    guard !preset.isTapeSpeed, let call = VoiceChanger(mode: .call, sampleRate: input.sampleRate) else {
        continue
    }
    call.setPreset(preset)
    var samples = input.samples
    let block = input.sampleRate / 100
    samples.withUnsafeMutableBufferPointer { buffer in
        var position = 0
        while position + block <= buffer.count {
            call.processCall(UnsafeMutableBufferPointer(rebasing: buffer[position..<(position + block)]))
            position += block
        }
    }
    try? Wav(sampleRate: input.sampleRate, samples: samples).write(folder.appendingPathComponent("call-\(preset).wav"))
}
print("Wrote \(folder.path)")
