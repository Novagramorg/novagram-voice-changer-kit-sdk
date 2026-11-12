// SPDX-License-Identifier: GPL-2.0-or-later

/// A voice changer preset. The raw values are the Novagram Android ids, which are stored in preferences: never renumber.
public enum VoiceChangerPreset: Int32, CaseIterable, Sendable {
    case off = 0
    /// A boy of 8-10 ("bola").
    case child = 1
    case woman = 2
    case man = 3
    case helium = 4
    case hexafluoride = 5
    case monster = 6
    case mouse = 7
    case robot = 8
    case alien = 9
    case echo = 10
    case cave = 12
    case hoarseness = 13
    case modulation = 14
    case noise = 15
    /// Tape speeds (message mode only; a call treats them as Off).
    case speed0_70 = 16
    case speed0_85 = 17
    case speed1_25 = 18
    case speed1_50 = 19
    /// New in v2: a 4-6 year old ("yosh bola").
    case littleKid = 20
    /// New in v2: a 14-17 year old girl ("yosh qiz").
    case youngGirl = 21
    /// New in v2: the voice snapped to the nearest semitone.
    case autoTune = 22

    /// Changes the length of a message, so it is never used in calls.
    public var isTapeSpeed: Bool {
        return (16...19).contains(rawValue)
    }

    /// Has settings in VoiceChangerParams (Modulation's tone, Echo's delay and volume, Noise's volume, Auto-tune's speed).
    public var hasSettings: Bool {
        switch self {
        case .modulation, .echo, .noise, .autoTune:
            return true
        default:
            return false
        }
    }

    /// Aims at a pitch from the speaker's own, so a low and a high voice come out alike.
    public var isPitchAware: Bool {
        switch self {
        case .child, .littleKid, .youngGirl, .woman, .man, .monster, .mouse:
            return true
        default:
            return false
        }
    }

    /// Needs the stage chain (ported separately) for its sound; without it only its pitch part plays.
    public var usesStageChain: Bool {
        switch self {
        case .robot, .alien, .echo, .cave, .hoarseness, .noise, .monster:
            return true
        default:
            return false
        }
    }
}
