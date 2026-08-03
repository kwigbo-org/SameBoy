#pragma once

/* Public module surface of the SameBoy core for Swift Package Manager
   consumers.

   GB_DISABLE_DEBUGGER must match how the target's sources are compiled (see
   Package.swift); gb.h derives GB_DISABLE_CHEAT_SEARCH from it, so the
   debugger and cheat-search declarations — whose definitions are excluded
   from the build — never appear in the module.

   GB_INTERNAL is deliberately NOT defined here: consumers get the opaque
   GB_gameboy_t handle API only. */

#ifndef GB_DISABLE_DEBUGGER
#define GB_DISABLE_DEBUGGER
#endif

/* SwiftPM never propagates the target's cSettings defines to importers, but
   undefine GB_INTERNAL here anyway so the opaque-handle guarantee is enforced
   by this header rather than assumed from build-system behavior. */
#ifdef GB_INTERNAL
#undef GB_INTERNAL
#endif

#include "../../../Core/gb.h"
#include "../../../Core/memory.h"
