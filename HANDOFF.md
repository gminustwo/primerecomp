# Handoff: 2026-10-21 (night)

Where the work stopped, and what to do next. Read with PROGRESS.md (history) and the commit log.

## State

- **Playable:** boot, menus, intro, frigate start, 60 / 120 fps. Saving and loading are verified: the game's own
  autosave was forced with `LARECOMP_AUTOSAVE_AT`, then reloaded after a restart.
- **The user's playtest build:** `private/playtest/MPRRecomp/` (gitignored: it contains the game library), started
  with `Play Metroid Prime Remastered.bat`. Their notes go in `ISSUES.md` there and their log is
  `lasrecomp.log`. That folder holds build 32af903, and the work below is **not** in it yet.
- **User-reported open bugs:**
  - The star backdrop and Tallon IV are missing in space.
  - Samus renders wrong.
  - Many objects are black.

## In progress: black space background (missing stars and planet)

The intro, debug frame 9000 (replay recipe below):

1. Pass 3 draws the scene, planet and stars included, into the HDR target. Pass 5 is fine too.
2. Pass 6 is the volumetric fog composite: fs `729bffd400`, a full-screen quad.
   - It blends `dst * src.a + src`.
   - It samples scene depth and the fog volume, a 100x57x64 RGBA16F 3D texture.
3. The fog volume is now written by the game's rasterizer-discard draw: 5700 points, vs `729bfea100`, `imageStore`
   into an image3D.
4. Readback of the volume (`LARECOMP_DUMP_3D=1` in the debug frame, `build/build/gpu3d_100x57x64_f29.bin`):
   - Slices 0-62 are correct: alpha (transmittance) about 0.98-0.9995, no NaN.
   - **Slice 63 is all zeros.** Sky pixels (far depth) sample the last slice, so the background is multiplied by 0.
     **This is the remaining bug.**
5. **Next step:** find out why slice 63 is never written.
   - Read the writer `build/build/glsl/729bfea100_vs.glsl` (run with `LARECOMP_SAVE_MSL=1`). Look at its slice
     loop: the bound, an off-by-one in a translated branch or predicate, or a store whose z-coordinate is computed
     as 64 (out of range, so dropped).
   - Or the composite clamps to slice 63 where the hardware would use the edge differently: compare its
     coordinate math in `729bffd400`.
   - A quick test: write slice 62's data into 63 as well (a hack), to confirm the background comes back.

## Fixed this session (all uncommitted until the commit that adds this file)

- **Sparse textures** (lighting volumes in virtual pools): `nvnMemoryPoolMapVirtual` page table, gathered upload,
  sparse 3D tiling. This fixed the black arm cannon and the black pipes.
- **The menu opened during play:** a Steam Controller in Steam's desktop mode sends Esc for B. Esc now opens the
  menu only without a controller.
- **FMZ multiplies** (`.FMZ` on FMUL/FFMA/FMUL32I/FFMA32I: 0 * anything = 0). Samus's gunship renders now.
- **Rasterizer discard** (`nvnCommandBufferSetRasterizerDiscard` was a no-op). The fog writer's 5700 points were
  being drawn into the scene.
- **Bindless image handles in the driver table:** a handle read from cbuf 2 past the game's bound size comes from
  the driver layout, texture slots at 0x20 + 8n and image slots at 0x120 + 8n. Without this, the fog writer had
  no image.
- **Debug aids:**
  - The debug frame logs pass attachments (`[pass]`), clears (`[clear]`), storage images (`image (stage`), and
    `RASTERIZER-DISCARD` on draws.
  - `LARECOMP_DEBUG_PREPASS=1`: the target as each pass finds it, saved as `pre_NNN.png`.
  - Pass images now show the target the pass actually rendered to. Before, they were off by one whenever the
    targets changed.
  - `LARECOMP_DUMP_3D` reads back GPU-written 3D textures.
  - `LARECOMP_NAN_SCRUB` zeroes NaN/inf fragment outputs (diagnostic only).
  - `LARECOMP_SAVE_MSL=1` dumps the translated GLSL to `build/build/glsl/`.

## How to work

- **Replay the intro to frame 9000:**
  `IN=2500:A:8,2800:A:8,...,5500:A:8` (A every 300 frames), then `LARECOMP_DEBUG_FRAME=9000` with the scratchpad
  `boot.sh <name> 175`.
  - The scratchpad scripts (`boot.sh`, `edit.py`) live in the previous session's scratchpad. If they're gone:
    build with `PATH=/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH make -j16`, then run
    `LARECOMP_NO_LAUNCHER=1 build/lasrecomp.exe --play private/game` from the repo root.
  - The runtime's cwd is `build/`, so its outputs land in `build/build/`.
- **Reading a debug frame:**
  - A `[draw] e/a` line is printed **before** its pass begins.
  - The `[pass]` line that follows it, and the `[draw] vp` line, belong to that draw.
  - `saved pass_N` closes pass N.
- **Gameplay:** frame ~12000 with the same input script. **Saves:** `LARECOMP_SAVE_DIR=<scratch dir>` and
  `LARECOMP_AUTOSAVE_AT=<frame>`.
- **Regenerating game code** (after `tools/patches.py` changes):
  `cd tools && ./venv/Scripts/python gen.py main ../private/exefs/main 0x7000000000 ../generated/main`, then `make`.
- **Updating the playtest build:** copy `build/lasrecomp.exe`, `build/lasgame.dll` and `build/lasgame.dll.id` into
  `private/playtest/MPRRecomp/`. This only works while the game isn't running.

## After the background

1. Black objects elsewhere and Samus's look: take an F9 / debug frame, then follow the pass-by-pass method above.
   Suspects:
   - more image stores through the driver handle table;
   - other rasterizer-discard producers;
   - `nvnCommandBufferDiscardColor`, which is treated as a clear to 0 (an LA assumption, `cmdbuf.c:314`).
2. Save slot area name `<ERROR>` (seen with a forced autosave): check it with a save made at a real save station.
3. Update the playtest folder and tell the user.
