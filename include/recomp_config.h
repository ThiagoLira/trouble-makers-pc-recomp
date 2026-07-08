#ifndef RECOMP_CONFIG_H
#define RECOMP_CONFIG_H

// Game-specific shim for the Mischief Makers recomp.
//
// The generated RecompiledFuncs/*.c do not currently include this header —
// they only need recomp.h (macros + recomp_context) and funcs.h (prototypes),
// both of which the runtime provides. This file exists as the home for any
// game-specific configuration the runtime integration grows to need in later
// phases (e.g. RECOMP_EXIT / mod-export macros, section counts, asset paths),
// mirroring how Zelda64Recomp carries a game-private include/ directory.
//
// Keep it free of generated code; this is hand-maintained glue, not output.

#endif
