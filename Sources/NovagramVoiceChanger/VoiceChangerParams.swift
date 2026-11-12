// SPDX-License-Identifier: GPL-2.0-or-later
import NovagramVoiceChangerCore

/// The settings a user can change, in the Android app's steps.
public struct VoiceChangerParams: Equatable, Sendable {
    /// Modulation: semitones, -12...12.
    public var tone: Int
    /// Echo: delay in tenths of a second, 1...10.
    public var echoDelay: Int
    /// Echo: volume in tenths, 1...10.
    public var echoVolume: Int
    /// Noise: volume in tenths, 1...10.
    public var noiseVolume: Int
    /// Auto-tune: 0 snaps instantly (the robotic effect), 1 is slow and natural.
    public var retuneSpeed: Float
    /// Man, woman, child, mouse and monster aim at a pitch from the speaker's own; false uses Android's fixed shifts.
    public var pitchAware: Bool

    public init(tone: Int = 5, echoDelay: Int = 5, echoVolume: Int = 7, noiseVolume: Int = 7, retuneSpeed: Float = 0.1, pitchAware: Bool = true) {
        self.tone = tone
        self.echoDelay = echoDelay
        self.echoVolume = echoVolume
        self.noiseVolume = noiseVolume
        self.retuneSpeed = retuneSpeed
        self.pitchAware = pitchAware
    }

    public static let `default` = VoiceChangerParams()

    var cValue: nvc_params {
        return nvc_params(
            tone: Int32(clamping: tone),
            echo_delay: Int32(clamping: echoDelay),
            echo_volume: Int32(clamping: echoVolume),
            noise_volume: Int32(clamping: noiseVolume),
            retune_speed: retuneSpeed,
            pitch_aware: pitchAware ? 1 : 0
        )
    }
}
