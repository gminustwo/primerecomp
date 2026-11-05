// Builds the game code library (runtime/gamelib.h) on the player's machine, from their own game files: the generator
// (tools/gen.py; shipped frozen as primegen/) turns exefs/main and exefs/sdk into C, the bundled compiler (zig/) builds
// that into <game_dir>/primegame.*. The C and the objects are deleted afterwards.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // now and then, from the calling thread: how far the whole build is (0..1) and what it's doing
    void (*progress)(double fraction, const char *step);
    int jobs;  // compiler processes at once (>= 1)
} GameBuildOpts;

// Builds it (a few minutes). false: why says what went wrong (details in the log).
bool gamebuild_run(const char *game_dir, const GameBuildOpts *opts, char *why, size_t why_size);

// The game code's x86-64 level for this CPU (issue #28): 3 with AVX2, FMA and BMI2 (built -march=x86-64-v3, as the
// Makefile's GENARCH), else 2 (x86-64-v2: SSE4.2 - slower, the ARM fused multiply-adds done in software); 0 on
// other CPUs. LARECOMP_CPU_LEVEL=2 pretends (tests).
int gamebuild_x86_level(void);
// The id file's text for a library built here: GAMELIB_ID, "-v2" after it for an x86-64-v2 build (so a copy moved
// between an older and a newer PC builds its game code again for that one)
const char *gamebuild_expected_id(void);

// True when the library at path is one this build loads: built with its GAMELIB_ID (the id file next to it; not
// loaded). In runtime/gamelib.c, declared here because gamelib.h is part of GAMELIB_ID (any edit to it has every
// player rebuild the game code after an update).
bool gamelib_current(const char *path);

#ifdef __cplusplus
}
#endif
