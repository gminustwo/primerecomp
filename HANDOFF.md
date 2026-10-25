# Handoff: 2026-10-25

Where the work stopped, and what to do next. Read with PROGRESS.md (history) and the commit log.

## State (2026-10-25)

- **The user is preparing a beta build ("PrimeRecomp").** They asked for, and got (both in the playtest build, wait
  for their reaction):
  1. **Name and icon** (8ae0b90): window / message boxes / start screen / packages say PrimeRecomp; docs/icon is
     Samus' helmet (SVG source; the .ico's 16-48 px sizes drawn without the rounded square's margin; Windows keeps
     the .exe's icon). The Windows exe is `primerecomp.exe` since 2026-10-25 (user's request), its import library
     `primerecomp.dll.a`: lasgame.dll imports from the exe by name, so a lasgame.dll linked before the rename doesn't
     load with the new exe (relinked; its GAMELIB_ID doesn't change). macOS / Linux binaries are still `lasrecomp`;
     the log / settings files are still lasrecomp.log / larecomp.ini.
     **The user's Steam shortcut must be retargeted to primerecomp.exe** (they were told how; Steam was running, so
     shortcuts.vdf was left alone). The old lasrecomp.exe is gone from the playtest folder.
  2. **The menu from a controller** (8ae0b90): L3+R3 opens / closes it (they tried that; Steam takes Guide). The
     overlay's Quit tab closes the game (the game's own pause menu also has Options -> Quit Game, to its title).
  3. **Mouse and keyboard like Halo** (33b4872): game.c `mouse_look` (from rt_player_frame, each gameplay frame):
     yaw = player transform turned about y via CActorMP1::SetTransform; pitch = player +0x5cc (current) / +0x5d0
     (desired), radians, up positive, +-70 deg, plus `ResetTo` on the IIR filter at [player+0x1628] (else the game
     averages it away). Gates: +0x4d4 morph state 0, +0x4e0 orbit state 0, +0x1610 bit 0 clear, first-person camera
     current; otherwise the motion goes to the right stick (ball camera). input.c: relative mouse mode while
     game_in_play (player updated within 2 frames) + focus + no overlay; mouse buttons / wheel are key bindings
     (BIND_MOUSE_*, 512+); beam slots 1-4 = X + d-pad dir; menu keys outside play (Enter / Space A, Esc / Backspace
     B, Q / E L / R, arrows d-pad), fixed at press time. Esc = Minus (pause) by default, so the overlay is F1 only.
     Old keyboard bindings (no `bind_keys = fps` line) are ignored: the user's LAS-era ones were.
     Tests: scratchpad tools/save_run.ps1 <tag> <secs> <LARECOMP_INPUT extra> <HOST_INPUT> (loads save2/);
     HOST_INPUT `mouse:<dx>/<dy>:<frames>`, `key:Mouse Left`; LARECOMP_LOOK_LOG=<n>.
     Not checked: beam keys 1-4 (the test save has only the Power Beam), real mouse capture by hand (scripted runs
     never capture), the feel of the default sensitivity (0.044 deg per count; Source's 2.0).
  - **Shipping the beta (next, ask the user how: a zip for testers? a GitHub release?):** `make dist` needs `make
    deps-windows` (LGPL FFmpeg), `make gen-helper` (frozen generator, python.org 3.12) and tools/fetch_zig.sh - none
    are built on this machine yet. VERSION comes from launcher.mm's LARECOMP_VERSION. Publishing is outward-facing:
    confirm first.
- **Landing Site lighting (64959de, in the playtest build since 2026-10-25):** ask the user whether the dark spots
  by the ship are gone. The door-shield check is done (frigate force field refraction unchanged, A/B).
- **Older notes (2026-10-23), the Landing Site lighting / rain report.** The user loaded
  their save (Landing Site, by the ship) and reported "lighting issues when I walk forward or around the area where
  I saved" (F9 capture: `private/playtest/MPRRecomp/build/f9_13830/`, log backed up in `log_backup/
  lasrecomp_2026-10-23_2131_f9.log`) and "the rain effect isn't always appearing on Samus' visor or the arm
  cannon". Their screenshot didn't come through; the F9 frame shows the arm cannon pitch black.
  - **Cause found (lighting):** MPR blends its two nearest reflection probes into a 128x128 R11G11B10F cube
    (CReflectionProbeManager::ResolveReflectionProbe; blend shader fs 729c00f800: out = probe1*cb3[0].x +
    probe0*cb3[0].y, per face via a passthrough-GS layer, per mip) whenever Samus is between probe zones. That cube is
    the scene's whole image-based lighting there - world and cannon (a shader override writing red turned
    everything red). The object the game samples it through can be made, and uploaded from (all-zero) memory,
    before the blend first renders; textures.c share_find refused to join an upload-only image, so the blend got
    an image of its own and the scene sampled zeros: black. Timing-dependent (some runs share, some don't).
  - **Fix, uncommitted when the user stopped me, now committed as WIP (see the log):** share_find(..., gpu_write)
    - an object about to be rendered into (renderer_mark_gpu_owned / vk_tex(for_render)) joins an uploaded image of
    its shape. Verified: the blend target joins (LARECOMP_LOG_TEX_SHARE: "joins ... uploaded"), its six faces and
    eight mips decode to the scene (scratchpad tools/cube_r11.py on a LARECOMP_DUMP_3D cube dump). Only two images
    change behavior on this route: the probe blend target and the refraction mip chain (@729215c000 1024x512) -
    the latter is worth a look at a blue door's shield (the refraction case the sharing was made for).
    In the blend zone the scene is now dim-but-lit (an overcast corner: probe0 or probe1 alone look the same as the
    blend, so the weights are right); crossing the zone edge is smooth. Installed 2026-10-25.
  - **Repro:** copy of the user's save in scratchpad save2/; scratchpad tools/probe_route.ps1 <exe> <wd> <tag>
    [root] (load, walk left from the ship into the blend zone at about (-363, -31, -310), turn in place, shots),
    probe_cross.ps1 (step back out: the blend stops at frame 7722), probe_edge.ps1. New logs: LARECOMP_LOG_CUBE_PASS
    (a pass into a cube target, per frame) and LARECOMP_LOG_TEX_SHARE also names upload-only images not joined.
    Shader overrides: LARECOMP_GLSL_OVERRIDE is relative to the data root (build/), so build/<dir> means
    build/build/<dir>; LARECOMP_VK_NO_SPIRV_CACHE=1 with it.
  - **Rain on the visor / cannon (not started):** "isn't always appearing". Likely related (the cannon's wet look
    may ride on the same lighting) or its own bug: check after the fix, at the ship in the rain.
  - Also seen in the user's log: 120 fps around frames 6482-7198 with frame_rate = 60 (unexplained, check).
- **Playable:** boot, menus, the whole frigate, onto Tallon IV (the user's own playthrough), 60 / 120 fps.
  Saving and loading work (a forced autosave reloads; frigate saves always restart at the landing site).
- **The user's playtest build:** `private/playtest/MPRRecomp/` (gitignored: it holds the game library), started
  from Steam (its shortcut targets primerecomp.exe; Steam Input applies). Their log is `lasrecomp.log` there, F9
  captures go to `build/f9_<frame>/`, F8 appends positions to `positions.txt`. It holds build 33b4872 (release
  exe, installed directly: the game was closed; `private/playtest/update/` holds the same build, so
  `Install_update.bat` is harmless). Use the update folder route whenever the game is running.
  `tools/run_game.ps1` now only waits for / stops build/primerecomp.exe, so tests can run while the user plays.
  (Its `KEY=VALUE` variables stay set in that PowerShell process: two runs in one call share them.)
- **The user confirmed the cannon sounds work** (2026-10-23 evening). Then asked for, in order: 4K working and as
  an option (done: 7fab0aa), the face buttons flippable to an Xbox layout (done: 7fab0aa), then "the recomp menu
  options - the LA-only ones don't make sense, focus on Metroid ones" (first pass: e12a7f6 - Gameplay tab with
  Field of view and Skip any cutscene; Aspect ratio / Movie audio out; Quit -> Restart Game). From my list of
  candidates they then picked **gyro aiming and sharper textures** (both done: dc25199, 2a4aaa3), then **ultrawide**
  (done: 52109d1, 87a6e17), then "the recomp menu / boot up screen: the options and the look" (done: 36d8f16 -
  theme.h palette for the start screen and the F1 menu, the start screen rebuilt for MPR). Wait for their reaction.
- **Start screen** (launcher.mm): since 2026-10-23 the user's Steam shortcut has no launch options (they wanted the
  start screen), so the exe finds the game as `extracted\` in the playtest folder: a junction to private\game (no
  copy; "Delete game files" refuses reparse points). F1 -> Quit -> Quit to Start Screen also reaches it
  (RT_REBOOT_TITLE; checked there and PLAY back).
  Background: the Tallon IV MP4 from UIMP1.pak (found by size, cached at cache/start_bg.mp4), decoded on the
  launcher's thread at 30 fps into a texture (vk_imgui_texture_update). Screenshots without booting the game:
  run build/primerecomp.exe private/game (no --play) with LARECOMP_LAUNCHER_SHOT=<frame>[:settings[:<tab>]|:setup|
  :tip|:fail<n>] and LARECOMP_SHOT_PATH. The red title-menu loop isn't a plain MP4 in any pak (compressed?).
- **Ultrawide** (Video -> Aspect ratio, Unlocked by default; wider windows only): game.c wide_frame writes the
  projection's aspect ([[display manager (state manager + 0x4e0168) + 0x10] + 0x98]); exec.c ui_boxed keeps UI drawn
  into presented targets (the HUD pass's HDR target counts) in the 16:9 box; edge_overlay lists the overlays whose
  outer edge goes to the window's edges (scan ring c8eaf61f..., helmet / pause backdrop 405a0731...) through the
  vertex shader variant (ShaderKey.x_mode bit 1); the minimap is a "UI layer" (a full-size RGBA8 target laid over the
  HUD by a 54-vertex mesh: that mesh is boxed). Unchecked: thermal / X-ray visors, damage / water / steam effects on
  the visor, text boxes from scans - a seam at the 16:9 edge means another overlay for edge_overlay: find its hash
  with a debug frame at LARECOMP_WINDOW=2560x720 (LARECOMP_LOG_FSHASH=1) and try LARECOMP_EDGE_FSHASH=<hash>.
  (Debug log order: a draw's "[draw] X inst" line, its textures, any "[pass]" lines, then its "vp" line.)
- **Gyro** (runtime/hle/motion.c): the game's six-axis imports are HLE'd from SDL's gyro / accelerometer. The user's
  Steam Controller reports both through Steam (log: "[input] Steam Controller: gyro yes, accelerometer yes"). In
  the game: Options -> Controls -> Control Scheme -> Pointer / Hybrid; it then calibrates (waits for "at rest").
  IsSixAxisSensorAtRest is the game's "put down" signal: true too often (a gyro of exactly 0 between test moves)
  makes it ask "The pointer will now be recentered" every few seconds; now 1.5 s of steadiness. Test runs reach
  Pointer through the pause menu: MINUS, L (Options), A, A, RIGHT, A, MINUS, then A for "calibrated" (see the
  2a4aaa3 commit message for the test variables). Ask the user how it feels; sensitivity is the game's own.
- **Texture filtering** (vulkan/textures.c vk_sampler): MPR makes only 13 samplers (one 2x, the rest trilinear);
  16x forced on the mipmapped linear ones by default. The FOV lookup crashed after a skipped cutscene (the state
  manager's game logic is CStateManagerGameLogicDefault between worlds): now needs the MP1 logic's vtable.
- **4K:** MPR presents 1600x900 (the Switch's docked image, the TV scales it to 1080p); output_scale took 1080 as
  the base, so Match display rendered 5/6 of the window (3200x1800 on the user's 4K screen - their log showed it).
  The scale now follows the presented image's size. Internal resolution: Match / Dynamic / 900p / 1080p / 1440p /
  4K / 8K (larecomp.ini `resolution = 2160p`). 4K checked on the user's save: same image as 900p, clean edges.
- **Face layout** (Controls tab, `face_layout = xbox`): bindings.c swaps south<->east, west<->north in the bindings
  and their defaults; menus confirm through bind_face_host. MPR draws its prompts as font letters (no texture
  glyphs to restyle), and with the Xbox layout those letters match an Xbox pad's labels.
- **FOV** (game.c fov_frame): the default first-person FOV is the unnamed float at 0x70020e7a70 (55.0; 0.2 / 750 /
  1.42 after it), read by CCameraManagerMP1::GetDefaultFirstPersonVerticalFOV. The live camera: camera manager =
  [[state manager + 0x4e0150] + 0x30], current camera id from its stacks, CCameraManagerMP1::GetCurrentCamera,
  vtable CFirstPersonCameraMP1. Calling it at boot (no player yet) crashed - guarded by the player's last update.
- **The user's save** (`private/playtest/MPRRecomp/save/save_0/save.bin`: **Save Station Magmoor A** - they went
  Chozo Ruins -> Magmoor; 3%, 0:33, 5 missiles) loads in test runs: copy it to `<scratch>/save_0/save.bin`, run
  with `LARECOMP_SAVE_DIR=<scratch>`, the intro replay's A presses and `LARECOMP_SKIP_CINEMATICS=1`: the save
  loads at frame ~4100, play from ~5500. The door ahead leads into Burning Trail (Chozo stone corridor); the
  elevator to Chozo Ruins North is up the corridor. MINUS opens the map (room names), PLUS the pause menu.
- **"If you load my save there are no cannon sounds"** - fixed in 01308e1 and confirmed by the user. Rumble added
  in 8f9ae7c (it was dropped): ask how it feels (Steam Controller through Steam Input).
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
- **Lead (mine, 2026-10-23): Burning Trail's lava as flat white-orange blocks.** Seen only by teleporting from the
  corridor (-162.69, 7.43, -445.03) to (-172 / -182, 7.43, -445) - outside the current room's collision (Samus
  falls through the world), so the room there may not be set up (lights / exposure) - possibly a test artifact.
  The lava is the last draw of the HDR scene pass: fs hash 6346b44e1a6739e8 (a standard material: normal t0,
  properties t1, albedo t2 = 2048x1024 BC6H, env cube t3), with fcb4[0] = (50, 50, 50) - very bright on purpose.
  Auto exposure works: CGameRenderJob::FeedLastFrameAverageRadianceBackToGame reads the 1x1 luminance level back
  every frame (R11G11B10F, (0.26, 0.11, 0.04) in the save room) and CPostFXManager::UpdateTonemapping converges
  on it. Ask the user whether lava looks flat white in play (an F9 + F8 there settles it), or walk there:
  the save station door, then the corridor to the left. Teleport survey of the corridor (y 7.43, z -445): solid
  from x -162.7 to -147 (warm orange-lit rock ahead at -147, facing +x = yaw 90); at x <= -172 or >= -142 the
  points are inside rock / other rooms (falls, or "stands" at y -8.09 inside a lava volume with the submerged
  visor effect) - useless as views. Points off z -445 (x -156..-150, z -435..-470) all fall: the walkable strip
  is narrow and its exits are at other heights (stairs?) - the route to the elevator / lava needs real play.
- The Burning Trail corridor looks greenish only toward the save station door (its cyan light); along the
  corridor the walls are warm orange as in the MPR references.
- Reflection probes: each room's materials bind a baked 128x128 BC6H cube (8 levels, uploaded: @72ca862400 in the
  save station, @72c616b600 in Burning Trail when walked in). After a teleport out of the room's bounds they get a
  1x1 placeholder cube instead - a teleport artifact, not a bug (it also explains odd colours in such views).

## Fixed on 2026-10-23

- **No cannon sounds (most sound effects in play silent):** MPR sends each sound through a splitter (voice mix
  0x7FFFFFFF + a splitter id; 5 destinations: the panned dry send to mix 3, a reverb send to mix 4). The splitter
  section ('SNDH') was ignored. Now parsed and routed for voices and sub mixes (`update_splitters`,
  `split_dest`); mixes ordered by their longest route over both kinds. The game's effects are ported from Ryujinx:
  I3DL2 reverb (type 5, the reverb send bus, dry gain 0 - passing it through would have doubled every send),
  delay (type 3, other rooms' bus), biquad (type 6, a shelf on mix 3's second buffer set). A/B:
  `LARECOMP_NO_SPLITTERS=1`, `LARECOMP_NO_EFFECTS=1`. Checked by WAV capture (`LARECOMP_AUDIO_WAV`) on the user's
  save. The renderer is REV9.
- **Aux on the final mix** (type 2): MPR's CAxalFinalMixEffect::AuxUpdate reads the send ring every frame, runs
  its processors and writes the return ring (measured: the input delayed 144 samples - a lookahead limiter).
  The send ring is fed; the output passes through (`LARECOMP_AUX_RETURN=1` reads the return: +23 ms, and stale
  samples whenever the game's audio thread misses a 5 ms frame). (fnprof doesn't count WriteAuxReturnBuffer:
  tiny SDK thunks aren't counted - don't conclude "never called" from a missing fnprof entry.)
- **Rumble** (8f9ae7c): hid 201 / 206 (SendVibrationValue(s)) -> SDL_RumbleGamepad, npad 0 + handheld, left
  device -> left motor. Idle frames send zeros (CRumbleManager "HardStop" -> CNXJoyCon_NPad::StopVibration every
  frame is normal); missiles give ~0.6. Menu: Audio -> Rumble; `LARECOMP_LOG_RUMBLE=1`.

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
    `LARECOMP_WINDOW_SHOT=<frame>` with `LARECOMP_EXIT_AFTER_SHOT=1` (the window, then exit), or
    `LARECOMP_HOST_INPUT=<frame>:shot:<name>` (the window, menus included, as build/build/<name>.png; also on Vulkan
    since 2026-10-23). The F1 menu: `LARECOMP_HOST_INPUT=6000:sdlkey:F1:5,6040:sdlkey:E:5` (E / Q: next / previous
    tab).
  - Sound: `LARECOMP_AUDIO_WAV=<path>` records the renderer's output (float stereo 48 kHz, a frame = 240 samples;
    `py -E tools/wav_stats.py <wav> [from to [step]]`: levels, clipping, clicks); `LARECOMP_NO_AUDIO=1` keeps the
    speakers quiet. `LARECOMP_AUDIO_LOG=1`: voices, splitters, effects, mix gains.
  - Contact sheets: `py -E tools/montage.py out.png <cols> <width> files...` (Pillow and numpy are in the user's
    Python, not in tools/venv; ffmpeg is in C:/msys64/ucrt64/bin).
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
  `LARECOMP_SHOT_DIR` separates parallel runs' shots. Several teleports in one run: `LARECOMP_TELEPORT=f1:x,y,z@yaw;
  f2:x,y,z@yaw;...` (yaw in degrees) - walk into a room through its door first (it is then lit as in play), then
  look around it. Turning: RSTICK_LEFT at full tilt turns 360 degrees in ~125 frames.
- **Regenerating game code** (after `tools/patches.py` changes):
  `cd tools && ./venv/Scripts/python gen.py main ../private/exefs/main 0x7000000000 ../generated/main`, then `make`.
- **Updating the playtest build:** the playtest exe is a release (GUI) build, started by Steam directly (its
  non-Steam shortcut targets primerecomp.exe (no launch options: start screen; extracted\ junction -> private\game), so
  Steam Input applies; it logs to lasrecomp.log itself). Link one with `rm build/primerecomp.exe && make RELEASE=1
  build/primerecomp.exe`, copy it and `build/lasgame.dll` / `.dll.id` into `private/playtest/MPRRecomp/` (only while
  the game isn't running), then relink the console dev exe (`rm build/primerecomp.exe && make`).
