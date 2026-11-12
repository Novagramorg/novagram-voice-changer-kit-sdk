// SPDX-License-Identifier: GPL-2.0-or-later
import NovagramVoiceChangerCore

/// One voice changer engine over mono 16-bit PCM.
///
/// Message mode: feed the recording chunk by chunk with `process(_:)` (the output size varies), then call `drain()`.
/// Call mode: change fixed blocks (10 ms) in place with `processCall(_:)`; `setPreset` may be called from any thread
/// mid-call and switches without a click. The process calls belong to one thread.
public final class VoiceChanger: @unchecked Sendable {
    public enum Mode: Sendable {
        case message
        case call
    }

    public let mode: Mode
    public let sampleRate: Int
    public let maxBlockSamples: Int
    private let engine: OpaquePointer

    /// - Parameters:
    ///   - sampleRate: 8000...192000.
    ///   - maxBlockSamples: call mode only, the largest block (default 10 ms).
    public init?(mode: Mode, sampleRate: Int, maxBlockSamples: Int? = nil) {
        let block = maxBlockSamples ?? sampleRate / 100
        let cMode = mode == .call ? Int32(NVC_MODE_CALL) : Int32(NVC_MODE_MESSAGE)
        guard let engine = nvc_engine_create(cMode, Int32(clamping: sampleRate), Int32(clamping: block)) else {
            return nil
        }
        self.mode = mode
        self.sampleRate = sampleRate
        self.maxBlockSamples = block
        self.engine = engine
    }

    deinit {
        nvc_engine_destroy(engine)
    }

    /// Chooses a preset. `stageChain` is the seam for the stages after the pitch engine (see nvc.h); the engine
    /// releases it once it is no longer used. Message mode: only before the first chunk.
    @discardableResult
    public func setPreset(_ preset: VoiceChangerPreset, params: VoiceChangerParams = .default, stageChain: nvc_stage_chain? = nil) -> Bool {
        var cParams = params.cValue
        if var chain = stageChain {
            return nvc_engine_set_preset(engine, preset.rawValue, &cParams, &chain) == Int32(NVC_OK)
        }
        return nvc_engine_set_preset(engine, preset.rawValue, &cParams, nil) == Int32(NVC_OK)
    }

    /// Call mode: the constant delay of the changed voice in samples, 0 while Off.
    public var latencySamples: Int {
        return Int(nvc_engine_latency_samples(engine))
    }

    /// The speaker's pitch in Hz, once about 0.2 s of voice was heard.
    public var speakerPitchHz: Float? {
        let value = nvc_engine_speaker_pitch_hz(engine)
        return value > 0 ? value : nil
    }

    /// The pitch shift currently applied, in semitones.
    public var shiftSemitones: Float {
        return nvc_engine_shift_semitones(engine)
    }

    /// Message mode: processes one chunk and returns what is ready (may be empty while the engine looks ahead).
    public func process(_ input: [Int16]) -> [Int16] {
        let bound = Int(nvc_message_output_bound(engine, Int32(clamping: input.count)))
        guard bound >= 0 else {
            return []
        }
        var output = [Int16](repeating: 0, count: bound)
        let written = input.withUnsafeBufferPointer { inputBuffer in
            output.withUnsafeMutableBufferPointer { outputBuffer in
                nvc_message_process_i16(engine, inputBuffer.baseAddress, Int32(inputBuffer.count), outputBuffer.baseAddress, Int32(outputBuffer.count))
            }
        }
        output.removeLast(output.count - max(0, Int(written)))
        return output
    }

    /// Message mode: everything still held after the last chunk.
    public func drain() -> [Int16] {
        var result: [Int16] = []
        var buffer = [Int16](repeating: 0, count: 8192)
        while true {
            let written = buffer.withUnsafeMutableBufferPointer { pointer in
                nvc_message_drain_i16(engine, pointer.baseAddress, Int32(pointer.count))
            }
            if written <= 0 {
                break
            }
            result.append(contentsOf: buffer[0..<Int(written)])
        }
        return result
    }

    /// Call mode: changes the block in place. Returns false for a wrong mode or a block over maxBlockSamples.
    @discardableResult
    public func processCall(_ samples: UnsafeMutableBufferPointer<Int16>) -> Bool {
        guard let base = samples.baseAddress else {
            return samples.isEmpty
        }
        return nvc_call_process_i16(engine, base, Int32(samples.count)) == Int32(samples.count)
    }

    @discardableResult
    public func processCall(_ samples: inout [Int16]) -> Bool {
        return samples.withUnsafeMutableBufferPointer { processCall($0) }
    }
}
