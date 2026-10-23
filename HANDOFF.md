# Handoff: 2026-10-22

Where the work stopped, and what to do next. Read with PROGRESS.md (history) and the commit log.

## State (paused for the night 2026-10-22; the user picks it up again tomorrow)

- **Playable:** boot, menus, the whole frigate, onto Tallon IV (the user's own playthrough), 60 / 120 fps.
  Saving and loading work (a forced autosave reloads; frigate saves always restart at the landing site).
- **The user's playtest build:** `private/playtest/MPRRecomp/` (gitignored: it holds the game library), started
  from Steam (its shortcut targets lasrecomp.exe; Steam Input applies). Their log is `lasrecomp.log` there, F9
  captures go to `build/f9_<frame>/`, F8 appends positions to `positions.txt`. It holds build a77199c (release
  exe; installed by the user with `Install_update.bat`, which copies `private/playtest/update/` in when the game
  is closed - use that route whenever the game is running).
- **Next, from the user:**
  1. "The door still is black" - which door is unknown (its capture was overwritten before F9 got its own
     folders). Ask for an F9 + F8 at it; then teleport there (LARECOMP_TELEPORT; far rooms also need
     LARECOMP_LOAD_AREAS and come up unlit) or analyse the capture.
  2. The corridor capture (cap2, frame 14099: blocky cyan patches on a wall, large black areas) predates the
     occlusion query fix; recheck with a new capture.
  3. Map fill and button glyphs fixed today: wait for the user's confirmation (shading had no reference).
- **User-reported open bugs (older):**
  - Many objects are black. The blue door shields (flat cyan) are fixed; no other black objects found in the
    first two rooms.

## Fixed on 2026-10-22

- **Space backdrop (stars, Tallon IV):** the fog volume's writer ends with `ISET.GE RZ.CC` then `@!P0 EXIT.NEU`.
  The translator dropped ISET's `.CC` and ignored the CC test on EXIT and BRA, so the writer always exited before
  its last slice (63), which the sky samples. Now: `cc_cond` (shared with CSETP), CC tests on EXIT / BRA, `.CC` on
  ISET, and on LOP / LOP3 / SHL / SHR / IMNMX / BFE / F2I / I2F / FSET / FADD / FMUL / FFMA through `set_reg`
  (`Ctx.cc_write`: not exercised by any shader seen so far).
- **Blue door shields (flat cyan):** MPR renders its refraction mip chain one level per texture object (eleven
  objects over one allocation) and the shield samples it through a twelfth. Each object had its own VkImage.
  Now textures with the same address and shape share one refcounted TexBackend (textures.c `share_find`), but only
  uncompressed formats, and never joining an image that holds CPU-uploaded contents. Sharing everything broke
  the room after the first door (stale and half-updated textures). `LARECOMP_NO_TEX_SHARE=1` turns it off.
- **Garbage colours over the room after the first door** (walk on past the door, frame ~13800): MPR builds its
  reflection probe (a 128x128 RGBA16F cube, one draw per mip covering all six faces) with a passthrough geometry
  shader that only sets the layer. Geometry shaders aren't translated, so faces 1-5 were never written; once the
  probe's objects shared one image, the sampled faces were uninitialized memory. Now `gs_layer_attr` (exec.c)
  recognises "layer = F2I(vertex output)" geometry shaders and the vertex shader writes gl_Layer
  (`ShaderKey.layer_attr`, `shaderOutputLayer`); array / cube render targets bind all their layers
  (`vk_render_layers`). New uncompressed colour images also start zeroed. Reflections show on the arm cannon and
  floors now.
- **The first shot of a new game was black** (nebula, asteroids, Tallon IV; user F9 capture): storage images got
  their view from `vk_view(render = true)`, which returned none for 3D images, so the fog writer's descriptor was
  never written and its stores went wherever a recycled set pointed (by luck the volume at frame 9000, nothing in
  that shot: the composite multiplied everything by 0). 3D images now get a 3D storage view. Vertex shaders also
  rescale texel fetches of scaled targets now (the fog writer reads one).
- **Samus in the landing cutscene was a burst of spikes**: skinning reads bone matrix rows with
  `LDC c[6][R + bone*0x30 - 0x10]`; the translator took the 16-bit offset as unsigned (+0xfff0), clamped to the
  palette's end. Signed now with a register base.
- **Untextured materials (the gunship one flat brown, no canopy; door shields flat cyan; deck stripes and the
  arm cannon's glow missing)**: materials pick their UV set with AL2P + IPA.IDX; the translator added IPA's
  immediate to AL2P's full address, read past every varying and sampled at UV (0, 0). IPA.IDX now uses the
  register alone (all 358 indexed IPAs seen take it from AL2P).
- **The rare freeze** (menus after starting a game, the intro, likely room loads - the user's "nothing rendered
  after the first door"): svc_wait_process_wide_key set the condvar key after releasing the mutex, so a guest
  thread taking the mutex in user mode in between skipped its signal: a lost wakeup. Caught by the new stall
  watchdog (thread dump after 10 s without a frame, on by default; LARECOMP_STALL_DUMP).
- **The black screen in the first enclosed corridor** (user F9 capture: the scene pass fine, the final image black):
  the fog writer's bindless image handle comes from hardware cbuf 2 (the driver table), but `image_view` indexed
  the game's buffers with the hardware number, and there the game had bound a buffer at index 2. Now c >= 3 is
  `ubuf[c - 3]`. Not yet seen in a run that reaches that corridor (scripted routes stop at the second force field;
  the user is to F8 a position there).
- **The user got past the frigate onto Tallon IV** (the elevator scans since the occlusion queries). Still open
  from them: "the door still is black" (which door: unknown - its F9 capture was overwritten; F9 now keeps each
  capture in build/f9_<frame>/).
- **The main menu stutter** ("very jittery", gameplay fine): one long frame (26 ms, then 7) every 2 s - the stats
  line: LOG was an unbuffered fprintf, which the Windows CRT writes in pieces (~0.4 ms a line, 8 ms for the stats).
  LOG now formats then writes once (rt_log). Ruled out on the way: the simulation (one tick every frame), the movies'
  clock (exactly 1/60 per frame), video decoding (60 fps, ahead of display), texture change detection, the pace
  mechanism. A "30 fps main menu" option was tried and dropped (its 1/30 step stuck into the game: the state manager
  logic's constructor reads the step). LARECOMP_TICK_LOG / LARECOMP_MOVIE_TIMING are the tools.
- **Button glyphs** (fragments of brackets): the default block height is the power of two nearest the height in GOBs
  (tegra.c; LARECOMP_BH_PROBE scored every texture between powers of two: 9 rows 1 GOB, 13 2, 26 4, 52 8).
- **The map's filled rooms** (wireframe only): geometry shaders are translated now (translate.c: VMAD / VSETP /
  ISBERD / ALD from a vertex / AST / OUT; exec.c: a third stage, set 2). LARECOMP_NO_GS=1 draws without them.
- **Occlusion queries** (the user: an elevator that won't scan, frame ~25700, position in positions.txt): the Scan
  Visor, lights and scene nodes are tested with SAMPLES_PASSED queries (CGraphicsOcclusionQueryToken_NVN); the report
  said 0, so small scan targets were "hidden" (on the landing deck lock-on skipped the force field's switch). Real
  Vulkan occlusion queries now (exec.c `oq_*`); `LARECOMP_OCCLUSION_ZERO=1` for A/B. Whether it fixes the elevator:
  the user's next session.
- **A freeze at the title screen** (main thread spinning in `GetTouchScreenStates`): QueryMemory reported the HLE
  stub range and the HLE heap free, so the SDK's random placement of the HID shared memory could land on host
  allocations. Both are kernel regions now.
- Debug: texture and attachment lines show the guest address (`@addr`), flags and refs; `LARECOMP_LOG_TEX_SHARE`
  logs each shared image join; draws with untranslated stages say so; the stats line counts textures / images.

## Open

1. ~~Intermittent hang~~: the condvar lost wakeup, fixed in 2012c44. If a freeze recurs, the log has a
   "[hang] no frame presented" thread dump. (Old notes:) once in about eight intro replays, the game froze just after frame 9097 (no log
   output, process alive). Not reproduced with `LARECOMP_HANG_DUMP=30` set. Next time: run with
   `LARECOMP_HANG_DUMP=30` and look at the guest threads' waits.
2. ~~The arm cannon is teal-green~~: correct - its material colour is white and the third-person model's cannon
   is the same grey-green. (Old notes:) Its albedo (tex2, ASTC sRGB 8x5) really is grey-teal and the shader multiplies
   it by `cb4[0].rgb` (a material colour), so the colour must come from that constant. Check against a reference
   image of the Power Suit in MPR before chasing it. `LARECOMP_DEBUG_DATA` printed the wrong buffers until 422cc4e (hardware cbuf c is
   `st.ubuf[stage][c - 3]`), so the "cb4 = 0 0 0" reading was wrong: look again.
3. ~~Frame 7000 of the intro replay is black~~: the fog volume bug, fixed in 422cc4e.
4. Save slot area name `<ERROR>` with a forced autosave: check with a save made at a real save station.
5. ~~Two other geometry shaders are still dropped~~: translated since fd5c33e (with the map's). (Old notes:) (`gs 729c002100`, `729c004100`, logged once per program with
   the frame: 5131 and 10558 of the intro replay). Full geometry shaders: the vertex shader only outputs world
   positions (v0), the GS projects the triangle with c[3] matrices, culls (EXIT) and emits (OUT, past the first
   0x800 bytes - dump more to see it). Their draws are depth-only into the main depth buffer and draw nothing
   without the GS; no visible difference found. A general GS translator (inputs per vertex via ISBERD/ALD,
   AST + OUT.EMIT/CUT -> EmitVertex/EndPrimitive, the VMAD/50f0 vertex-index prologue) would cover them. Their code: `LARECOMP_SAVE_MSL=1` writes
   `build/build/glsl/<addr>_stage2.bin` (0x30 NVN header, 0x50 SPH, then instructions).
6. Uploads: 50-170 MB every 2 s in gameplay, 500 MB at the title (the video?). Frame rate holds; not looked at.
7. Navigating the frigate by script: the door after the landing site is opened by locking on (ZL) to its four
   red lights and firing (ZR); R fires a missile. A 12030 RSTICK_LEFT:5 turn, then ZL/ZR cycles, opens it by
   frame ~12660. Appended to the intro replay's LARECOMP_INPUT (PowerShell):
   `$c = @(); foreach ($b in 12100,12180,12260,12340,12500,12580,12660,12740) { $c += "${b}:ZL:60";
   foreach ($o in 15,27,39,51) { $c += "$($b + $o):ZR:5" } }; $c += "12430:RSTICK_LEFT:8"`, then walk with
   `12850:STICK_UP:<frames>`.
8. Suspects for other black objects: more image stores through the driver handle table, other
   rasterizer-discard producers, `nvnCommandBufferDiscardColor` treated as a clear to 0 (an LA assumption,
   `cmdbuf.c`).

## Fixed on 2026-10-21 (night)

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

- **Build:** `PATH=/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH make -j16` (Git Bash).
- **Run tests from PowerShell with `tools/run_game.ps1 <log> <seconds> KEY=VALUE...`** (the intro replay example
  is in the script). Started from Git Bash with the MSYS2 directories first on PATH, the game silently runs
  without the `LARECOMP_*` variables, and `timeout` misses it after it re-executes itself.
  - Intro replay: `LARECOMP_INPUT` with A every 300 frames from 2500 to 5500; frame 9000 is in space.
  - Pictures: `LARECOMP_SHOT_AT=<frame>,...` (the game image, `build/build/frames/`), or
    `LARECOMP_WINDOW_SHOT=<frame>` with `LARECOMP_EXIT_AFTER_SHOT=1` (the window, then exit).
  - The runtime's cwd is `build/`, so its outputs land in `build/build/`.
- The title screen's attract demos (no input, from frame ~6000) are 30 s videos: they test the movie player
  (fine), not the renderer. Later areas need real play or saves.
- **Reading a debug frame:**
  - A `[draw] e/a` line is printed **before** its pass begins.
  - The `[pass]` line that follows it, and the `[draw] vp` line, belong to that draw.
  - `saved pass_N` closes pass N.
- **Gameplay:** frame ~12000 with the same input script; add `12100:STICK_UP:200` to walk through the first
  door (frame 12500 is the next room). **Saves:** `LARECOMP_SAVE_DIR=<scratch dir>` and
  `LARECOMP_AUTOSAVE_AT=<frame>`.
- **Getting somewhere fast** (2026-10-22): `LARECOMP_SKIP_CINEMATICS=1` skips every cutscene (a new game is at the
  landing site, (91.38, 0.25, -105.23), by frame ~5600 with only the menu A presses). `LARECOMP_TELEPORT=0:x,y,z[,9
  rotation values]` teleports at the first gameplay frame; F8 in game appends the player's position as such a value
  to `positions.txt` (cwd). `LARECOMP_POS_LOG=50` logs positions. y is up. Teleporting by guessed coordinates mostly
  fell into space (points off the deck or inside the hull); use F8 positions. Whether a teleport into another room
  (another area) needs `LARECOMP_LOAD_AREAS` (the room is then solid but unlit: not set up as through its door), and
  doors keep their game state (the upper hangar door: "cannot be opened until room is repressurized"). Frigate
  saves always load at the landing site, even with `LARECOMP_SAVE_AREA`. D-pad right is the Scan Visor in
  `LARECOMP_INPUT` (pressed after the HUD has booted, ~frame 5900 with skipped cutscenes), ZL scans.
  `LARECOMP_SHOT_DIR` separates parallel runs' shots.
- **Regenerating game code** (after `tools/patches.py` changes):
  `cd tools && ./venv/Scripts/python gen.py main ../private/exefs/main 0x7000000000 ../generated/main`, then `make`.
- **Updating the playtest build:** the playtest exe is a release (GUI) build, started by Steam directly (its
  non-Steam shortcut targets lasrecomp.exe with `--play "D:\Games\pcports\MPRRecomp\private\game"`, so Steam
  Input applies; it logs to lasrecomp.log itself). Link one with `rm build/lasrecomp.exe && make RELEASE=1
  build/lasrecomp.exe`, copy it and `build/lasgame.dll` / `.dll.id` into `private/playtest/MPRRecomp/` (only while
  the game isn't running), then relink the console dev exe (`rm build/lasrecomp.exe && make`).
