# SPDX-License-Identifier: GPL-2.0-or-later
load("@rules_cc//cc:objc_library.bzl", "objc_library")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# The voice changer DSP: plain C, no Telegram module and no Apple framework. Used by the Telegram-iOS recorder
# (Swift) and call audio (TgVoipWebrtc). No external dependency: NovagramPitchShift.c is our trimmed copy of Sonic
# (CREDITS.md).
objc_library(
    name = "NovagramVoiceDSP",
    enable_modules = True,
    module_name = "NovagramVoiceDSP",
    srcs = [
        "Sources/NovagramVoiceDSP/NovagramVoiceDSP.c",
        "Sources/NovagramVoiceDSP/NovagramPitchShift.c",
        "Sources/NovagramVoiceDSP/NovagramPitchShift.h",
    ],
    hdrs = [
        "Sources/NovagramVoiceDSP/include/NovagramVoiceDSP/NovagramVoiceDSP.h",
    ],
    includes = [
        "Sources/NovagramVoiceDSP/include",
    ],
    copts = [
        "-O2",
    ],
    visibility = [
        "//visibility:public",
    ],
)

# The same sources for the host test (a cc_test can't depend on an objc_library).
cc_library(
    name = "NovagramVoiceDSPHost",
    srcs = [
        "Sources/NovagramVoiceDSP/NovagramVoiceDSP.c",
        "Sources/NovagramVoiceDSP/NovagramPitchShift.c",
        "Sources/NovagramVoiceDSP/NovagramPitchShift.h",
    ],
    hdrs = [
        "Sources/NovagramVoiceDSP/include/NovagramVoiceDSP/NovagramVoiceDSP.h",
    ],
    includes = [
        "Sources/NovagramVoiceDSP/include",
    ],
)

# Every preset runs, tape speeds change the length as expected, nothing clips, and the call path keeps each
# buffer's size.
cc_test(
    name = "NovagramVoiceDSPTests",
    srcs = [
        "Tests/NovagramVoiceDSPTests.c",
    ],
    deps = [
        ":NovagramVoiceDSPHost",
    ],
)
