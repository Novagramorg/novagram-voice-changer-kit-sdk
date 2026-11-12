# SPDX-License-Identifier: GPL-2.0-or-later
load("@build_bazel_rules_swift//swift:swift.bzl", "swift_library")
load("@rules_cc//cc:objc_library.bzl", "objc_library")

# The C++ engine and the plain C API (include/nvc.h, module NovagramVoiceChangerCore). No dependencies.
# Built with -O2 even in debug builds: the spectral code is ~20x slower at -O0, too slow for a call's audio thread.
objc_library(
    name = "NovagramVoiceChangerCore",
    module_name = "NovagramVoiceChangerCore",
    enable_modules = True,
    srcs = glob([
        "Sources/NovagramVoiceChangerCore/*.cpp",
        "Sources/NovagramVoiceChangerCore/*.hpp",
        "Sources/NovagramVoiceChangerCore/spectral/*.hpp",
    ]),
    hdrs = [
        "Sources/NovagramVoiceChangerCore/include/nvc.h",
    ],
    includes = [
        "Sources/NovagramVoiceChangerCore/include",
    ],
    copts = [
        "-std=c++17",
        "-O2",
    ],
    visibility = [
        "//visibility:public",
    ],
)

# The Swift preset enum, params and wrapper class.
swift_library(
    name = "NovagramVoiceChanger",
    module_name = "NovagramVoiceChanger",
    srcs = glob([
        "Sources/NovagramVoiceChanger/**/*.swift",
    ]),
    copts = [
        "-warnings-as-errors",
    ],
    deps = [
        ":NovagramVoiceChangerCore",
    ],
    visibility = [
        "//visibility:public",
    ],
)
