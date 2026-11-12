// swift-tools-version:6.0
// SPDX-License-Identifier: GPL-2.0-or-later
import PackageDescription

// No dependencies: everything the engine needs is in this repository (see CREDITS.md for what was adapted from where).
let package = Package(
    name: "NovagramVoiceChangerKit",
    platforms: [
        .iOS(.v15),
        .macOS(.v10_13)
    ],
    products: [
        .library(name: "NovagramVoiceChangerCore", targets: ["NovagramVoiceChangerCore"]),
        .library(name: "NovagramVoiceChanger", targets: ["NovagramVoiceChanger"])
    ],
    targets: [
        // The C++ engine behind the plain C API in include/nvc.h.
        .target(
            name: "NovagramVoiceChangerCore",
            path: "Sources/NovagramVoiceChangerCore"
        ),
        // Swift preset enum, params and a wrapper class.
        .target(
            name: "NovagramVoiceChanger",
            dependencies: ["NovagramVoiceChangerCore"],
            path: "Sources/NovagramVoiceChanger"
        ),
        // Renders every preset of an input WAV into samples/ for listening.
        .executableTarget(
            name: "nvc-render",
            dependencies: ["NovagramVoiceChanger"],
            path: "Sources/nvc-render"
        ),
        .testTarget(
            name: "NovagramVoiceChangerTests",
            dependencies: ["NovagramVoiceChanger", "NovagramVoiceChangerCore"],
            path: "Tests/NovagramVoiceChangerTests"
        )
    ],
    swiftLanguageModes: [.v6],
    cxxLanguageStandard: .cxx17
)
