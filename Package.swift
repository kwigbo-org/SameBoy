// swift-tools-version: 5.9
// SPM distribution of the SameBoy emulator core (kwigbo-org fork).
//
// The SameBoyCore target mirrors exactly what `make _ios` compiles for the
// core: Core/*.c minus the iOS CORE_FILTER set, built with GB_DISABLE_DEBUGGER
// (gb.h derives GB_DISABLE_CHEAT_SEARCH from it). Core objects are compiled
// with GB_INTERNAL, matching the Makefile's Core/%.c.o rule; the public module
// surface (Core/include) does NOT define GB_INTERNAL, so consumers get only
// the opaque-handle API.
//
// The target path is scoped to Core/ — never widen it to "." with a sources
// filter: SwiftPM auto-scans the entire target path for bundle resources
// regardless of `sources`, and the repo root contains macOS xibs (Cocoa/,
// QuickLook/, ...) that hard-fail iOS builds ("macOS xibs do not support
// target device type"). Core/ holds only .c/.h/.inc, so there is nothing for
// the resource scan to mis-process.
//
// Scope guardrail: only Core/ is distributed. iOS/ and HexFiend/ are excepted
// from the repository LICENSE and must never be added to a package target.

import PackageDescription

let package = Package(
    name: "SameBoy",
    platforms: [
        .iOS(.v17),
        .macOS(.v13),
    ],
    products: [
        .library(name: "SameBoyCore", targets: ["SameBoyCore"]),
    ],
    targets: [
        .target(
            name: "SameBoyCore",
            path: "Core",
            exclude: [
                // iOS CORE_FILTER set (Makefile `_ios` target)
                "debugger.c",
                "sm83_disassembler.c",
                "symbol_hash.c",
                "cheat_search.c",
                // .inc tables are textually #included by gb.c/sgb.c
                "graphics",
            ],
            publicHeadersPath: "include",
            cSettings: [
                .define("GB_INTERNAL"),
                .define("GB_DISABLE_DEBUGGER"),
                // Keep in sync with version.mk
                .define("GB_VERSION", to: "\"1.0.3\""),
                .define("_GNU_SOURCE"),
                .define("_USE_MATH_DEFINES"),
                .headerSearchPath("."),
                .headerSearchPath("../AppleCommon"),
            ]
        ),
    ],
    cLanguageStandard: .gnu11
)
