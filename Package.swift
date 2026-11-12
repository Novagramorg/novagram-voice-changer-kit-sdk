// swift-tools-version:5.9
// SPDX-License-Identifier: GPL-2.0-or-later
import PackageDescription

let package = Package(
    name: "NovagramVoiceChangerKit",
    products: [
        .library(name: "NovagramVoiceDSP", targets: ["NovagramVoiceDSP"]),
    ],
    targets: [
        .target(
            name: "NovagramVoiceDSP",
            path: "Sources/NovagramVoiceDSP",
            publicHeadersPath: "include"
        ),
        // `swift run NovagramVoiceDSPTests`: exits non-zero on a failure.
        .executableTarget(
            name: "NovagramVoiceDSPTests",
            dependencies: ["NovagramVoiceDSP"],
            path: "Tests"
        ),
    ]
)
