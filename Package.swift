// swift-tools-version: 5.9
// SPM distribution of the SameBoy emulator core (kwigbo-org fork).
//
// The SameBoyCore target mirrors exactly what `make _ios` compiles for the
// core: Core/*.c minus the iOS CORE_FILTER set, built with GB_DISABLE_DEBUGGER
// (gb.h derives GB_DISABLE_CHEAT_SEARCH from it). Core objects are compiled
// with GB_INTERNAL, matching the Makefile's Core/%.c.o rule; the public module
// surface (SPM/SameBoyCore/include) does NOT define GB_INTERNAL, so consumers
// get only the opaque-handle API.
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
            path: ".",
            exclude: [
                // iOS CORE_FILTER set (Makefile `_ios` target)
                "Core/debugger.c",
                "Core/sm83_disassembler.c",
                "Core/symbol_hash.c",
                "Core/cheat_search.c",
                // .inc tables are textually #included by gb.c/sgb.c
                "Core/graphics",
            ],
            sources: ["Core"],
            publicHeadersPath: "SPM/SameBoyCore/include",
            cSettings: [
                .define("GB_INTERNAL"),
                .define("GB_DISABLE_DEBUGGER"),
                // Keep in sync with version.mk
                .define("GB_VERSION", to: "\"1.0.3\""),
                .define("_GNU_SOURCE"),
                .define("_USE_MATH_DEFINES"),
                .headerSearchPath("."),
                .headerSearchPath("AppleCommon"),
            ]
        ),
    ],
    cLanguageStandard: .gnu11
)
