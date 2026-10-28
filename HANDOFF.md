# Handoff: 2026-10-27

Where the work stopped, and what to do next. Read with PROGRESS.md (history) and the commit log.

## State (2026-10-25)

- **PICK UP HERE (stopped 2026-10-27 late; the user: "create a handoff for now and well pick it up tomorrow").**
  Open, in the user's order - details in the bullets below:
  1. **Ultrawide (GitHub issue #1): fixed in local main only (07d6e85)** - not in the user's Steam copy, not in the
     v0.0.4 draft, not pushed. Ask the user: put it into v0.0.4 (rebuild the zip / AppImage with
     build/session_69905a54/dist_win.sh and appimage.sh, replace the draft's assets, add a line to its notes) and
     their copy? The squish itself was never reproduced: the reporters offered photos / video - a capture of it, or
     where exactly, would settle whether the fix covers it.
  2. **Crates should be scannable** (the user, while playing): which room? Unknown, and whether plain crates are
     scannable on the Switch is unconfirmed (a web search found nothing). Ask where; then compare the scan visor
     there with the game's data for that actor.
  3. **Magmoor's rising lava doesn't burn** (the user: "a room in magmoor caverns where you traverse in ball form and
     the lava rises ... the lava on the ground level behaves correctly"): probably Pit Tunnel (Lava Lake <-> Triclops
     Pit). No way there yet: the user's latest save is Phendrana Drifts' Save Station B
     (private/playtest/PrimeRecomp-9584b5a/save/save_0, 2026-10-27 22:49); the older Magmoor one (Save Station Magmoor
     A, next to Burning Trail) is in private/playtest/PrimeRecomp/save. Asked the user for a save near the room or an
     F8 in it (teleports: LARECOMP_TELEPORT + LARECOMP_LOAD_AREAS, "How to work"). Likely shape: the lava's damage
     volume not following the risen surface (water / trigger bounds), a script or recompilation bug.
  4. Waiting on the user: the Deck (branch deck-test = 1abae74: doors, pause screen, no Uncapped), the v0.0.4 dmg on
     their Mac, publishing v0.0.4 (then answer issues #1 and #2 - #2, "no option to disable vsync", is the v0.0.4
     V-Sync option; post nothing on GitHub without the user).
  - **Where things are:** local main e1dfe02 (+ this note) is ahead of every remote branch; origin/main 5e3f2db,
    deck-test 1abae74, release-v0.0.4 9e0d513 (the v0.0.4 draft's target). The user's Steam shortcut runs
    private/playtest/PrimeRecomp-9584b5a/, its exe 1abae74's release build (V-Sync option, door fixes, pause-screen
    fix, no Uncapped; old exe *.bak-9584b5a). **Test runs are muted** (LARECOMP_NO_AUDIO=1: the user hears them).
    This session's scripts (scratchpad 69905a54 may be gone) are in build/session_69905a54/: dist_win.sh (Windows zip
    with v0.0.3's tested zig/), appimage*.sh (WSL container build and tests, run from PowerShell: Git Bash mangles
    /mnt paths), release_v004_notes.md, vsemu/harness.cpp (a translated GLSL shader body compiled as C++ on a
    LARECOMP_DUMP_DRAW dump), skin.py / proj.py (CPU skinning and projection of a dump), nanprobe.py (first NaN / inf
    line of a fragment shader, written out as the colour; use with LARECOMP_GLSL_OVERRIDE), retarget.py (the Steam
    shortcut's path in shortcuts.vdf).
- **START HERE (2026-10-26).**
  - **Ultrawide fixes (07d6e85, 2026-10-27**, GitHub issue #1: two players at 32:9 5120x1440 and 3440x1440 - "the
    screen will force 16:9 and squish the picture at random points" (first: entering the frigate as the blue lights
    come on; fog-like visor effects; the escape sequence), "the scanning interface also squishes those elements",
    "the fade-to-white ... only in the center 16:9"). Seen here at LARECOMP_WINDOW=2560x720: the intro's fades to
    white and the explosion flash boxed (CGameCameraFilter quads, fs 6054ad08...), the visor power-on scan lines
    boxed, the map's backdrop boxed, the scan lens an oval half as wide. Fixed: exec.c edge_quad (boxed draws of up to
    24 vertices over the viewport get the edge stretch; all boxed draws smear HUD pieces at visor power-on), ui_boxed's
    wide-camera test (vertex cbuf 3's |m11/m00| = the window's aspect: drawn through the game camera -> fills: the
    scan lens and ring), c4028b01... (power-on scan lines) in edge_overlay. Checked at 32:9 and 21:9: title, save
    slots, HUD, scan visor, map, pause screens; 16:9 is untouched by construction (ui_boxed returns at once).
    **Not reproduced: the squish itself** (walked into the first frigate room and turned left toward the lights at
    32:9 vs 16:9: the middle 16:9 matched every 25 frames). Most likely a visor effect (CVisorSteam / camera blur)
    drawing a half-size copy of the scene as one full-screen quad - boxed before, edge-stretched now - but unseen.
    Scan markers on objects (DrawScanObjectIndicators) use the HUD's 16:9 camera; whether they sit right at 32:9 is
    unchecked (the lock-on targets differed between the 16:9 and 32:9 runs). Tools: LARECOMP_LOG_WIDE=1|<from>:<to>,
    "[draw]   wide:" in the debug frame, LARECOMP_EDGE_QUADS, LARECOMP_FILL_FSHASH, LARECOMP_SKIP_COUNT /
    LARECOMP_DUMP_DRAW. Not in the user's copy or a release yet. Next from the user: crates should be scannable;
    Magmoor's rising lava (morph ball room) doesn't burn.
  - **v0.0.4 is a DRAFT release** (2026-10-27, the user: "build a new zip release and app image for me and push those
    to a new branch"; asked, they chose a draft release + a code branch over committing the binaries): branch
    **release-v0.0.4** (the version bump 9d732e2 and this note), draft https://github.com/gminustwo/primerecomp/releases
    (v0.0.4, target release-v0.0.4; notes: scratchpad release_v004_notes.md of session 69905a54).
    PrimeRecomp-v0.0.4-windows.zip (sha256 b1ec708a...93dc): the dist recipe with v0.0.3's tested zig/ (scratchpad
    dist_win.sh; same 144 files as v0.0.3, only exe / dll.a / README differ). PrimeRecomp-v0.0.4-linux-x86_64.AppImage
    (sha256 cbe2feeb...8676): WSL container as before (scratchpad appimage.sh). v0.0.3's assets moved to
    build/dist-v0.0.3/. Checked: the zip extracted fresh sets up from an earlier copy (READY, game code shared), loads
    the user's save at 120 fps, pause screen clean; the AppImage in WSL (llvmpipe) on ~/prt/data2 loads its game code,
    intro at ~26 fps, no faults, GPU ASTC selftest 0 texels differ. No macOS dmg yet (needs the user's Mac: make dmg);
    the notes point Mac users at v0.0.3's until then - take that line out when the dmg is added. Publishing, when the
    user says so: `gh release edit v0.0.4 --draft=false --latest` (creates the tag), and push main.
  - **No more Uncapped frame rate (2026-10-27**, the user: "its working pretty well right now. One thing, remove the
    uncapped FPS option"): the Video tab's Frame rate is 30 / 60 / 120; the mode is gone from the code too (game.c's
    per-frame tick and UNCAPPED_MAX_FPS, device.c's FIFO at Uncapped). V-Sync stays: on MAILBOX, off IMMEDIATE.
    An ini's `frame_rate = 0` loads as 120; LARECOMP_FPS takes 30 / 60 / 120 only. The two bullets below about
    Uncapped are history. Checked: an ini with 0 plays at 120 fps, sim 120 Hz, speed 1.00x, mailbox; the menu's
    Frame rate stops at 120.
  - **Branch deck-test on GitHub (2026-10-27**, the user: "push to a branch I can try on my deck"): local main with
    this session's work (Uncapped fix, V-Sync option, the door-hitch commits, the pause-screen fix) merged with
    origin/main's macOS docs commit (5e3f2db). main itself isn't pushed: origin/main stays at 5e3f2db. Waiting on the
    user's Deck results: doors (README "Deck slowdowns at doors" stays until then) and the pause screen.
  - **Samus on the pause screen fixed (2026-10-27**, the user: "fix the render issues with samus' character models on
    the pause screen"): spikes from her left hand. Cause: the translator's LDC.64 with the address register among
    its destinations (`LDC.64 R24, c[4][R24-0x18]`) read the second word through the first one's loaded value
    (translate.c: the address is now held in `ldc_addr_`). In MPR's skinning shaders that is bone 3's x row, so every
    vertex with 3-4 bone weights was off (64 of 15154 in the upper body); 5 skinning shaders on the pause screen had
    it, likely the same ones as for skinned models elsewhere. Found with LARECOMP_SKIP_COUNT=<count>,... (skip draws
    by vertex / index count) and LARECOMP_DUMP_DRAW=<count> (debug frame: that draw's indices, streams, vertex cbufs
    as build/draw_<count>_*), then the translated GLSL compiled as C++ and run on the dump (scratchpad vsemu/: the
    body of main() #included in a harness) against numpy skinning: 0 of 15154 differ after the fix. README item
    crossed out. Looked at and left: the doll's dull, dark look (no reference to compare; her env cube is a 1x1
    black one the game clears itself - [cclear] now logs address, target and colour), thin orange edge lines on her
    armour (NaN probe of her fragment shader: none; stippled at 720p test windows, thin lines at 4K - may be the
    game's rim light), and a thin cyan diagonal line from the arm cannon in the Burning Trail (there before the fix).
    In the user's Steam copy: its exe (and dll.a, README) is f6eb888's release build, swapped in place in
    private/playtest/PrimeRecomp-9584b5a/ (the old ones kept as *.bak-9584b5a); checked there on the pause screen.
  - **Door hitches (2026-10-27**, the user: "jitter and frame drops when going through doors ... this is still pretty
    bad on steam deck performance too. so continue working on that"). Measured at the Magmoor save station's door
    (the user's save; script: intro A presses, ZR at 5900/5960/6020, STICK_UP from 6150 at 60 fps): the frame the next
    room shows was 293 + 261 ms, ~500 ms of ASTC decoded on the CPU on the render thread (~50 textures of 1024x1024 at
    ~10 ms). Fixed, in order: b74525d ASTC decoded by a compute shader (astc_gpu.c; bit-exact with astc.c:
    LARECOMP_ASTC_SELFTEST / LARECOMP_ASTC_CHECK), a1c05a4 sparse lighting volumes uploaded page by page (were 10-70
    ms each at every mapping change; LARECOMP_SPARSE_CHECK) and sampled textures without dedicated allocations (14-42
    ms vkAllocateMemory each), e91aadd shader builds below the game's threads. Now at 120 fps on the RTX 5090 / i9:
    door worst frame 11-17 ms (was 293), save load 95-227 ms (was 800+; the rest is the transient ring growing on
    the first load - pre-allocating chunks would help - and GPU waits). Tools: LARECOMP_VK_PROF=1 with
    LARECOMP_HITCH_MS=<ms> (per-frame categories, image / view creation), LARECOMP_LOG_SLOW_UPLOADS=<ms>,
    LARECOMP_COMPILE_THREADS, LARECOMP_INPUT_CLOCK. **Not run on a Deck**: the README's "Deck slowdowns at doors"
    stays until the user checks. A Windows Deck stand-in (6 E-cores) showed 25-70 ms gaps everywhere, title included,
    that look like Windows scheduling on parked cores, not the game (no busy threads: 32% of one core in play).
    Left on a door: first-use BC texture deswizzle + hash on the render thread (1-2.5 ms per 2048x2048 here); the GPU
    could read them in place from the imported heap (exec.c guest_direct) and deswizzle like the ASTC path.
  - **Uncapped frame rate fixed (2026-10-27**, the user: "turning off vsync doesn't actually turn off vsync"; they
    didn't say which switch - the game has no V-Sync option, Uncapped is the nearest). The game runs one simulation
    tick per frame however short the frame (CGameArchitectureSupport::Update's loop always ticks once), so Uncapped's
    1/60 tick ran the game ~6x fast at 400 fps. Now each frame's tick is its own time (game.c frame_time), up to 1000
    fps (game_pace_present), time a short frame ran ahead taken off the next. Measured at Uncapped (RTX 5090, ~400-530
    fps in play): speed 1.00x (new on the stats line), walk 10.8 u/s and jump peak 3.68 as at 60. Without mailbox
    (Mac / Deck?), Uncapped now presents IMMEDIATE instead of FIFO; the swapchain is remade on switching to or from
    Uncapped. Test hook LARECOMP_INPUT_CLOCK=60 (script frames as 1/60 s of time).
  - **V-Sync option (2026-10-27**, the user: "I want you to add a vsync option"; they want "as fast as is stable,
    but 120fps or 144 fps is fine", tearing OK; their screen is 4K 143 Hz). Video tab, `vsync` in the ini, on by
    default. On: MAILBOX as before, but FIFO at Uncapped (= the display's refresh: 144.0 fps here). Off: IMMEDIATE
    (tearing; Uncapped ~570 fps on the title). The swapchain is remade on a change (tested from the menu: FIFO ->
    immediate mid-run, saved). Acquire / present waits count as blocked (exec.c vk_pace_frame: a 60 Hz FIFO's 16.7 ms
    frames would have looked too slow and set the 30 fps mode). FIFO frame times jitter (under 5 / over 9 ms in turn
    at 144 Hz), so Uncapped's tick is now the average frame time (~16 frames), pulled back to real time: step sd 0.22
    ms vs 4.0 ms measured. Gameplay at V-Sync on + Uncapped: 144 fps, 1.00x, walk and jump as at 60. Not in the
    user's copy yet.
  - **v0.0.3 is the full release (Latest) and now has a macOS asset** (2026-10-26, on the user's Mac; the user: "I
    pushed a release for this from windows but it didn't include the mac release. fix it"): PrimeRecomp-v0.0.3-macOS.dmg
    (sha256 f0949185...cff51), `make dmg` from the v0.0.3 tag (97cd7e8) on the M3 Max. GAMELIB_ID on the Mac is
    95d45970a8df7ad6, the same as the user's existing Mac setup (no recompile). Checked: the app copied out of the dmg
    set up a fresh data root from exefs / romfs (READY, game code in 88 s with the bundled primegen and zig), then boots
    to the title and the frigate's landing deck at 60 fps, 0 skipped draws. **A downloaded copy is quarantined** (each
    file, once copied out of a quarantined dmg): spctl rejects the app, and the bundled zig and primegen are killed
    when run (exit 137); after `xattr -dr com.apple.quarantine` all three run. So the release notes, README (new
    "macOS (Apple Silicon)" in Getting started / Updating / Requirements) and docs/macos.md tell Mac users to run that
    command after installing and after each update. Untested: whether Open Anyway alone covers the bundled tools. A
    code-side fix for later: the app clearing the quarantine flag from its own Resources before running them.
  - **v0.0.3 is RELEASED, Latest** (2026-10-26, the user: "make v0.0.3 the full release"; a pre-release before that):
    https://github.com/gminustwo/primerecomp/releases/tag/v0.0.3, tag v0.0.3 on 97cd7e8 (created by publishing). Same assets as the draft below. Notes gained two lines (scratchpad release_v003_notes.md of
    session 67cd6949): the gunship's glass (dual-source blending, 8d3dc15) and shaders compiling again on the first
    play after updating (the tint removal). The macOS dmg came later (the bullet above).
  - **main is pushed** (2026-10-26, the user's request). **Bugs and feature requests go to the Discord**
    (https://discord.gg/TmWPy28wf), not GitHub issues: README "Bugs and feature requests", CONTRIBUTING.md and the
    v0.0.3 notes say so. **README's known-issues box** (the WARNING under "Status") is the user's list; they asked to
    cross items out (`~~...~~`) as they're fixed, so do that when one is: Deck slowdowns at doors, graphics /
    rendering issues, lighting, Samus' model on the pause screen. The release notes' Known issues match it.
  - **v0.0.3 rebuilt from main** (was a draft; the user: "rebuild the v0.0.3 release from main. I will want to test
    on my mac and steamdeck so don't publish the release yet. push to a branch I can pull from on those systems"):
    branch **release-v0.0.3** on GitHub (local main = that branch; origin/main is still 138318a - main itself isn't
    pushed). The draft now targets the branch head, with new assets and notes (scratchpad release_v003_notes.md):
    PrimeRecomp-v0.0.3-windows.zip (sha256 d73a6abc...6c91) and PrimeRecomp-v0.0.3-linux-x86_64.AppImage (sha256
    72174b2f...396e). The zip's zig/rt runtime libraries are the ones from the earlier, end-to-end-tested draft zip
    (zig_slim.sh rebuilds them differently each time; same zig and flags): its game-code toolchain is byte for byte
    the tested one; exe, import library and README are new. Checked: the zip without MSYS2 sets up from an earlier
    copy and plays the landing site at 60 fps; the AppImage in WSL (llvmpipe) on ~/prt/data2's v0.0.2-era setup -
    start screen, title, same Linux game code id (976eb3a7795b1f1d, no rebuild), dma-buf import falls back (no
    /dev/udmabuf in WSL). Not run: a fresh setup's full game-code build (the freeze below), the Deck's dma-buf path
    (the user's own test), macOS (the user builds PrimeRecomp.app from the branch: docs/macos.md). The previous
    draft's files are in build/dist-adb1c28/. Publishing, when the user says so: `gh release edit v0.0.3
    --draft=false --latest` (creates the v0.0.3 tag at the target), and push main.
  - **Don't publish anything else** until the user says so. Nothing pushed besides release-v0.0.3.
  - **The user's Steam Deck work is merged** (2026-10-26, the user: "merge the changes from the steam-deck branch
    onto here"): origin/steam-deck-dmabuf, one commit (6d073e9, authored fhclimited), merged as bf306fb. It
    builds on 8675408 (my Deck work) instead of replacing it: the guest heap as udmabuf dma-bufs on Linux
    (exec.c gmem_import_dmabuf; vkQueueSubmit no longer walks a 3.3 GB userptr buffer), Linux write tracking
    (userfaultfd + PAGEMAP_SCAN, platform/linux.c), and ui_run handling the waiting events (at most 256) and then
    the rest of the loop. On their Deck: render 15.2 -> 8.4 ms a frame standing, 15.4 -> 7.3 ms walking and
    shooting, 60 fps held. Nothing in it duplicates 8675408, so nothing was dropped. Checked here: Windows build
    without warnings; Windows keeps the host-pointer import (no dma-buf extensions on its drivers) and plays the
    landing site at 60 fps, 0 skipped draws, the same image. Not run here: Linux (needs the Deck or a udmabuf
    system). The remote steam-deck-dmabuf branch was then deleted at the user's request (2026-10-26): 6d073e9 is only in local main until main is pushed.
  - **Updates keep the game files** (6636919, 2026-10-26, the user: "make it so that when new release updates occur,
    the user doesn't need to re-install the game files"): the start screen rebuilds outdated game code by itself; a
    new Windows copy in another folder takes the latest earlier copy's files (hard links), game code (same id),
    saves and settings, found through %LOCALAPPDATA%\PrimeRecomp\copies.txt (paths.h rt_note_copy /
    rt_earlier_copies); picking an old PrimeRecomp folder (v0.0.2's too) does the same. README "Updating". To ship
    it, the draft v0.0.3's binaries need rebuilding from main (they're from adb1c28). Test hook:
    LARECOMP_LAUNCHER_EXTRACT=auto (what the start screen starts by itself); test runs with LOCALAPPDATA pointed at
    a scratch folder keep the user's copies.txt clean.
  - **The machine froze (2026-10-26) during a full-load game code build** (32 compiles; release copy, the
    auto-rebuild test): the user had to restart. The rebuild was not run to its end afterwards - ask before another
    full-load build here (gamebuild.c already notes this CPU's compilers crashing under full load).
  - **GAMELIB_ID is the same since v0.0.2** (2f2eef2dc358333a): updating from v0.0.2 needs no recompile. Any edit to
    GAMELIB_SRCS (comments too; gamelib.h, cpu.h, tools/*.py ...) makes every player recompile after updating.
  - **The Deck's 30 fps preset stays the first-start default** (settings.c settings_load ->
    settings_steam_deck_preset(30)); the user, 2026-10-26: "the 30fps mode doesn't seem to make much difference but
    we'll keep it for now".
    The playtest build (Windows) isn't updated with the merge: on Windows only ui_run's event loop changed.

- **The macos branch is merged into main** (2026-10-25 late, the user's request: "pull the macos branch and merge
  those changes here"): squashed as 8d3dc15, authored fhclimited - the branch's own commits (89a36ed, 9f72a52,
  fb26909) carry a personal name and email, which this repo keeps out of main. At the user's request the remote
  macos branch was then deleted (2026-10-25), and so were the local backups holding old identities (strip-la,
  backup-before-identity-rewrite, refs/original). The user's Mac lacks the fhclimited git identity: they were told
  to set it there. "Don't publish anything yet" (the user): v0.0.3 stays a draft, nothing pushed after 138318a. Checked here: Windows build without warnings, the Landing Site save at 60 fps with the same image
  as before, no skipped draws; Linux AppImage builds and boots to the title (llvmpipe) on a v0.0.2 setup. Game
  library inputs unchanged. The **draft v0.0.3 release is pinned to adb1c28** (its binaries predate the merge);
  for the macOS work in a release, rebuild the zip / AppImage from main (and PrimeRecomp.app on a Mac: docs/macos.md).
- **macOS build (the user's request, 2026-10-25 night, on their M3 Max / macOS 27; branch `macos`, merged with
  main's v0.0.3 work, not on main yet - the user merges it with their Windows / Linux work):**
  `make app` -> build/PrimeRecomp.app (macOS 15+). macOS builds the **Vulkan renderer on MoltenVK**; LAS Recomp's
  Metal renderer is deleted. `make deps` builds everything into build/deps from pinned sources (SDL3, LGPL FFmpeg,
  Vulkan headers / loader 1.4.350.1, VMA 3.3.0, MoltenVK 1.4.2's release dylib, shaderc 2026.4, Python 3.12); plain
  `make` uses build/deps once it exists. The user's real data folder (~/Library/Application Support/
  io.github.primerecomp) is set up and they have played the frigate on it (their save there; game code id
  95d45970a8df7ad6). Details: docs/macos.md.
  - MoltenVK fixes, all shared code: the GLSL prelude's fmin / fmax / clz are fminnm / fmaxnm / clz32 (Metal's own
    names made calls ambiguous: pipelines failed); **dual-source blending**: RT1's output is the second colour
    (location 0 index 1, ShaderKey.rt_kind[1] = RT_KIND_DUAL; as Ryujinx reads Maxwell) - it was undefined on every
    renderer (the gunship's glass, fs @729ae0b000-ish in the intro); no blending into targets a shader doesn't write;
    **geometry shaders without geometryShader** (ShaderKey.gs_lib, exec.c gs_emul_sources): the vertex shader runs a
    straight-line 3-emit GS on its own vertex - the pause map's and the HUD minimap's room fill (gs @729bfe7400) now
    draw on the Mac; the culling depth-only GS (@729c002100) is still left out. (A fragment-shader route through
    pervertexEXT was tried first: MoltenVK has barycentrics but no per-vertex inputs.) Checked on MoltenVK and, for
    the PC / Deck path, on Mesa's lavapipe (`brew install mesa`, VK_DRIVER_FILES=/opt/homebrew/share/vulkan/icd.d/
    lvp_icd.aarch64.json): map fill and dual-source both fine there. Linux / Windows compile-checked with zig
    (scratchpad crosscheck.sh: `zig cc -target x86_64-linux-gnu.2.35 / x86_64-windows-gnu` over runtime/), not run.
  - SPIR-V caches rebuild once after this update everywhere (the tint removal changed every fragment shader).
  - Test runs: run the .app's binary with `perl -e 'alarm N; exec @ARGV'` (no `timeout` on macOS; a killed run skips
    the pipeline cache save), LARECOMP_DATA_ROOT=<scratch> with `extracted` a symlink to the real one, and
    LARECOMP_SAVE_DIR; in zsh write `${f}:A`, not `$f:A`. Screenshots stall ~350 ms each at 2560x1440.
- **Cleanup (the user's request, same night):** removed the Metal renderer (metal.m, renderer.m, RT_METAL UI
  branches, MetalFX setting, test_shaders.*), texpatch.c/.h (LA texture patches: "0 known textures" in MPR) and the
  per-draw tint uniform in every fragment shader, the LA settings (prompts, tunic, hearts, hide_hero_icon,
  rando_seed), LARECOMP_REBOOT_AT's seed modes (now game | title), the LA opening-movie skip and Plus injection,
  DOORJUMP_TEST / PLACE, dead functions (maxwell_disassemble was an empty stub, vk_frame_extent, vk_defer_view,
  vk_format_alpha_one, renderer_get_scale, menu_is_open, nvn_find_pool, gamebuild_available,
  kernel_current_thread_handle, g_vsync_count), tools/run.sh + dbgfs.sh; macOS / Linux share runtime/platform/posix.c
  (log file, fatal box, child processes); CONTRIBUTING.md rewritten for PrimeRecomp; the log's first line is
  `[version] PrimeRecomp <v> (<os>, <arch>)` (tools/log_triage.py reads it). Kept on purpose: hle_override,
  kernel_data_regions, the RomFS overlay, option_rows / osk widgets without callers, the translator's MSL output path.
  Open: tools/test_translate.sh needs a REF commit from LAS Recomp's history. device.c logs the present mode
  (`[vk] present mode ...`; MoltenVK offers only fifo / immediate and holds 60 on fifo).
- **Steam Deck performance (2026-10-25 night, the user's request):** "testing on the steam deck now. the performance
  was abysmal. can you create a performance mode on here specific for steam deck? It was chugging at 25 frames". No
  Deck here, so the causes were measured on Windows with Linux's conditions (`LARECOMP_NO_WRITE_WATCH=1`, the Deck's
  1280x800 window; scratchpad tools/perf_run.ps1, speed_run.ps1 + speed.py) and fixed for every platform:
  1. **Sparse textures without write tracking** (textures.c): MPR's sparse arrays are 29 / 58 MB virtual with 1-2 MB
     mapped; every check (each 8th frame) gathered and hashed the whole thing: ~4.9 ms a frame on the i9. Now a
     sample of the mapped words decides, the mapped pages are hashed in place (sparse_mapped_hash) for 64 frames
     after a mapping change and as a safety net every 240 frames per 16 MB mapped; gathered only when that changed.
     Upload checks: 491 -> ~35 ms per 2 s. `LARECOMP_LOG_SPARSE` logs these checks.
  2. **CPU and GPU took turns** (exec.c, objects.c): copy_texture_to_buffer (MPR's average radiance for its exposure,
     every frame: CGameRenderJob::FeedLastFrameAverageRadianceBackToGame -> CStagingBufferPool) flushed and waited
     for the GPU; and nvnSyncWait with timeout 0 - the game's NVNUtil::is_sync_complete polls - waited too. Now
     readbacks go to buffers of their own and into guest memory when their submission is done (rb_complete: polls,
     waits, each present), and a poll answers at once (renderer_poll_seq: 2 = TIMEOUT_EXPIRED while running; an
     unsubmitted fence polled 64 times with nothing submitted waits as before). Waits: ~190 -> 3 ms per 2 s.
     Exposure unchanged in shots. `LARECOMP_VK_SYNC_READBACK=1` / `LARECOMP_SYNC_POLL_WAIT=1`: the old ways.
     With both (Linux-like run on the 5090, whose GPU reads vertices in place across PCIe: GPU 10 ms): 50 -> 60 fps.
  3. **Slow motion below 60 fps** (game.c frame_time, the rt_frame_step patch site): the game adds at most one
     tick of elapsed time per frame, so 25 fps was 42% speed. Now up to 3 ticks (LARECOMP_MAX_TICKS), snapped to
     whole ticks within a quarter tick (a steady 60 / 30 stays exactly 1 / 2 ticks a frame).
     Checked by the stats line's new `sim N Hz` (simulation ticks per second): a 30 ms stall per frame
     (LARECOMP_SLOW_FRAME_MS=30, ~30 fps) keeps 58-60 Hz; with LARECOMP_MAX_TICKS=1, 29-30 Hz (the old slow motion).
     (Walking-distance tests are useless for this: LARECOMP_INPUT counts frames, so turns at 30 fps go twice as far.)
  4. **The pacing's busy time counted the frame limiter's sleep** (game_pace_present: now in g_blocked_ns), so the
     LA-era "30 fps mode" counter faking (pace_slow) was on all the time in MPR. Harmless: MPR's dynamic resolution
     didn't change the image either way (A/B shots identical); the stats line now says "pace 60, 0 frames over".
  5. **Frame rate 30** (Video tab; ini frame_rate = 30): 33.3 ms frames, two ticks each. Checked: sim 60 Hz, two
     ticks a frame, and the movies' clock (CMovieList::Update) steps per tick, so videos keep their speed.
  6. **Steam Deck presets** (Video tab, section "Steam Deck"; settings.c settings_steam_deck_preset): steady 30 fps
     (Match display = 1280x720 on the Deck, 4x aniso, no SMAA) and 60 fps (75% = 960x540 + FSR 1, 2x aniso). A
     Deck's first start (no primerecomp.ini) takes the 30 one; detection: SteamDeck=1 or DMI Valve Jupiter / Galileo.
  7. **Show FPS** shows game / render / GPU ms per frame (exec.c vk_pace_frame: the update thread's busy time from
     rt_frame_step, the presenting thread's minus GPU waits, the submissions' GPU timestamps - always on now); the
     log's `[vk] t=` lines have `per frame: game .. render .. gpu .. ms`. On the i9 / 5090 at 1280x720: 2.5 / 4.5 /
     0.4 ms at the Landing Site.
  **v0.0.3 is a DRAFT release** (commit 8675408; notes: scratchpad release_v003.md): PrimeRecomp-v0.0.3-windows.zip
  (sha256 c5fb48dd...fd21; checked: fresh setup from the zip READY in 85 s, boot at 60 fps / sim 60 Hz) and
  PrimeRecomp-v0.0.3-linux-x86_64.AppImage (sha256 2ab8c79e...679c). Not published: the user is asked first
  (publishing makes it Latest and creates the v0.0.3 tag: `gh release edit v0.0.3 --draft=false --latest`).
  Ask the user for the overlay's numbers on the Deck (and its log): that says what limits it now. The game library's
  inputs didn't change, so a v0.0.2 setup on the Deck stays valid (checked: the v0.0.3 AppImage in WSL loads
  ~/prt/data2's v0.0.2 primegame.so, title screen fine; llvmpipe 25 fps at sim 57 Hz).
- **v0.0.2 is released** (2026-10-25 evening, the user's request: "bump it to v0.0.2", README for Linux, their Buy Me
  a Coffee link https://buymeacoffee.com/rengar - README badge + support section, .github/FUNDING.yml):
  https://github.com/gminustwo/primerecomp/releases/tag/v0.0.2, tag v0.0.2 on 9fa288a, assets
  PrimeRecomp-v0.0.2-windows.zip (sha256 68225bf5...5c1a) and PrimeRecomp-v0.0.2-linux-x86_64.AppImage (sha256
  bcf460e7...6d43). Both checked before publishing: fresh setup READY and the title screen (Windows from the zip
  without MSYS2, build/disttest_v002; Linux in WSL on llvmpipe). (The user's uncommitted v1.0.0 bump became v0.0.2.)
  **The user is testing the AppImage on a Steam Deck next:** ask how it went (setup in Desktop Mode, Game Mode,
  controls, frame rate) and for ~/.local/share/primerecomp/primerecomp.log. GitHub's Sponsor button also needs
  Settings -> General -> Features -> Sponsorships ticked (the user was told).
- **Linux / Steam Deck AppImage (the user's request, 2026-10-25 evening):** first built as v1.0.0, released as
  v0.0.2 (above). Built with the existing pipeline (tools/linux/: Ubuntu
  22.04 podman container) inside the user's WSL2 Ubuntu 24.04 (podman installed there as root; build tree in
  /root/primerecomp, synced from this checkout by scratchpad linux/sync.sh - rsync without build/ private/ generated/,
  CRs stripped from the shell scripts and the Makefile). Tested in WSL on llvmpipe only (no GPU Vulkan there): start
  screen, a fresh setup (READY, game code in 106 s), title screen, the user's save in Chozo Ruins. Fixes it needed:
  textures.c's missing sys/stat.h; the guest heap import capped at the device's maxMemoryAllocationSize /
  maxBufferSize (llvmpipe: 2 GB; the 3.3 GB import made vertex fetches empty - logo / movie / button glyph missing on
  the title); stencil ops set once per command buffer even with the test off (validation VUID 07848). Linux log now
  keeps primerecomp.previous.log and its fatal box shows the error line. Windows: LARECOMP_NO_WRITE_WATCH=1 runs
  without page write tracking, as Linux / macOS do. Untested: real GPUs (RADV on the Deck), the folder dialog in
  Game Mode (docs/linux.md says: set up in Desktop Mode), Deck performance. The README and docs/linux.md have the
  Steam Deck steps. Next release's AppImage, as root in WSL (`wsl -d Ubuntu -u root`): rsync this checkout to
  /root/primerecomp without build/ private/ generated/ .git/ tools/venv/ .venv/, strip CRs from tools/**/*.sh,
  tools/linux/AppRun and the Makefile, then `cd /root/primerecomp && podman run --rm -v /root/primerecomp:/src -w /src
  localhost/primerecomp-build:amd64 tools/linux/package.sh x86_64` -> build/dist/ there (the image, deps/ and
  helper/ are already built, so it takes a few minutes).
- **The user is preparing a beta build ("PrimeRecomp").** They asked for, and got (both in the playtest build, wait
  for their reaction):
  1. **Name and icon** (a1324f5): window / message boxes / start screen / packages say PrimeRecomp; docs/icon is
     Samus' helmet (SVG source; the .ico's 16-48 px sizes drawn without the rounded square's margin; Windows keeps
     the .exe's icon). The Windows exe is `primerecomp.exe` since 2026-10-25 (user's request), its import library
     `primerecomp.dll.a`: primegame.dll imports from the exe by name, so a primegame.dll linked before the rename doesn't
     load with the new exe (relinked; its GAMELIB_ID doesn't change). Then (same day, user's request: "rename any
     references to the LA port") the log is primerecomp.log (+ .previous.log; the last primerecomp.log becomes the
     previous log once), the settings primerecomp.ini (larecomp.ini renamed and saved again on first start, without
     LA's keys), the game library primegame.dll, the generator helper primegen (tools/primegen.spec), macOS / Linux
     names likewise (bundle id io.github.primerecomp, ~/.local/share/primerecomp; untested). LA-only leftovers removed:
     the dead settings (cheats, randomizer, difficulty, tracker...), the hidden Cosmetics tab, d-pad walking, the
     Linux randomizer helper (now tools/linux/gen_helper.sh) and LA smoke test. Kept on purpose: LARECOMP_* variables,
     internal symbols (lasgame_tables) and code comments about LA's history (upstream cherry-picks).
     **The user's Steam shortcut must be retargeted to primerecomp.exe** (they were told how; Steam was running, so
     shortcuts.vdf was left alone). The old lasrecomp.exe is gone from the playtest folder.
  2. **The menu from a controller** (a1324f5): L3+R3 opens / closes it (they tried that; Steam takes Guide). The
     overlay's Quit tab closes the game (the game's own pause menu also has Options -> Quit Game, to its title).
  3. **Mouse and keyboard like Halo** (7dd445a): game.c `mouse_look` (from rt_player_frame, each gameplay frame):
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
  - **v0.0.1 was released** (2026-10-25, the user's request; v0.0.2 above is the latest): https://github.com/gminustwo/primerecomp/releases/tag/v0.0.1,
    tag v0.0.1 on 4b1e840, asset PrimeRecomp-v0.0.1-windows.zip (sha256 d470ad62...2d4c, built by `make dist` from
    that commit). Checked before publishing: fresh setups from the zip reach READY (build/disttest*, one with
    LARECOMP_GAMEBUILD_FLAKY=1), and the packaged exe, with no MSYS2 on PATH, boots to the title screen at 60 fps.
    No game files in the zip. For the next release: bump launcher.mm's LARECOMP_VERSION and the README's version /
    zip name, `make dist` (deps-windows, gen-helper and zig are built now), retest a fresh setup, then tag and
    `gh release create`.
- **Landing Site lighting (9ed0ce8, in the playtest build since 2026-10-25):** ask the user whether the dark spots
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
- **The user's Steam shortcut now targets `private/playtest/PrimeRecomp-9584b5a/`** (2026-10-27, the user: "can
  you update the steam shortcut for me?"): release exe from 9584b5a (V-Sync option, GPU ASTC, sparse volumes by page,
  background shader builds) with v0.0.3's DLLs / zig / primegen; game files, game code, saves, settings via
  LARECOMP_LAUNCHER_EXTRACT=auto from the v0.0.3 copy, which stays as the fallback, and its shader cache copied.
  Checked there: the user's save (scratch copy) through the Magmoor door at 120 fps, worst frame 10 ms, load 82 ms.
  shortcuts.vdf backup: shortcuts.vdf.bak-before-9584b5a. Not on the Deck yet (nothing pushed).
- **The user's Steam copy was the v0.0.3 release** (2026-10-26 night, the user: "extract it and retarget my steam
  shortcut for me. keep my saves from the old test one"): `private/playtest/PrimeRecomp/` (the release zip,
  extracted; its save/ and primerecomp.ini copied from the old playtest folder, the 39 KB save_0; game files and
  game code hard-linked from build/dist/PrimeRecomp by LARECOMP_LAUNCHER_EXTRACT=auto: READY, no rebuild). The
  Steam shortcut (userdata/1612431156/config/shortcuts.vdf, backup shortcuts.vdf.bak-before-v003 beside it) now
  targets its primerecomp.exe, Start In that folder. Its log, F9 captures and positions.txt are there now. It's a
  release layout: the old update/ + Install_update.bat route doesn't apply; new builds go in by extracting a
  zip into another folder (README "Updating"). The old folder below stays as it was.
- **The user's playtest build:** `private/playtest/MPRRecomp/` (gitignored: it holds the game library), started
  from Steam (its shortcut targets primerecomp.exe; Steam Input applies). Their log is `primerecomp.log` there, F9
  captures go to `build/f9_<frame>/`, F8 appends positions to `positions.txt`. It holds build 7dd445a (release
  exe, installed directly: the game was closed; `private/playtest/update/` holds the same build, so
  `Install_update.bat` is harmless). Use the update folder route whenever the game is running.
  `tools/run_game.ps1` now only waits for / stops build/primerecomp.exe, so tests can run while the user plays.
  (Its `KEY=VALUE` variables stay set in that PowerShell process: two runs in one call share them.)
- **The user confirmed the cannon sounds work** (2026-10-23 evening). Then asked for, in order: 4K working and as
  an option (done: 3242f35), the face buttons flippable to an Xbox layout (done: 3242f35), then "the recomp menu
  options - the LA-only ones don't make sense, focus on Metroid ones" (first pass: e4e173a - Gameplay tab with
  Field of view and Skip any cutscene; Aspect ratio / Movie audio out; Quit -> Restart Game). From my list of
  candidates they then picked **gyro aiming and sharper textures** (both done: 351955a, 99095a7), then **ultrawide**
  (done: 98939ba, 6a5673c), then "the recomp menu / boot up screen: the options and the look" (done: 46ef506 -
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
  99095a7 commit message for the test variables). Ask the user how it feels; sensitivity is the game's own.
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
- **"If you load my save there are no cannon sounds"** - fixed in 6ac22d7 and confirmed by the user. Rumble added
  in 9030dd6 (it was dropped): ask how it feels (Steam Controller through Steam Input).
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
- **Rumble** (9030dd6): hid 201 / 206 (SendVibrationValue(s)) -> SDL_RumbleGamepad, npad 0 + handheld, left
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

1. ~~Intermittent hang~~: the condvar lost wakeup, fixed in 2990649. If a freeze recurs, the log has a
   "[hang] no frame presented" thread dump. (Old notes:) once in about eight intro replays, the game froze just after frame 9097 (no log
   output, process alive). Not reproduced with `LARECOMP_HANG_DUMP=30` set. Next time: run with
   `LARECOMP_HANG_DUMP=30` and look at the guest threads' waits.
2. ~~The arm cannon is teal-green~~: correct - its material colour is white and the third-person model's cannon
   is the same grey-green. (Old notes:) Its albedo (tex2, ASTC sRGB 8x5) really is grey-teal and the shader multiplies
   it by `cb4[0].rgb` (a material colour), so the colour must come from that constant. Check against a reference
   image of the Power Suit in MPR before chasing it. `LARECOMP_DEBUG_DATA` printed the wrong buffers until 63d020d (hardware cbuf c is
   `st.ubuf[stage][c - 3]`), so the "cb4 = 0 0 0" reading was wrong: look again.
3. ~~Frame 7000 of the intro replay is black~~: the fog volume bug, fixed in 63d020d.
4. Save slot area name `<ERROR>` with a forced autosave: check with a save made at a real save station.
5. ~~Two other geometry shaders are still dropped~~: translated since 6f29b43 (with the map's). (Old notes:) (`gs 729c002100`, `729c004100`, logged once per program with
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
  Steam Input applies; it logs to primerecomp.log itself). Link one with `rm build/primerecomp.exe && make RELEASE=1
  build/primerecomp.exe`, copy it and `build/primegame.dll` / `.dll.id` into `private/playtest/MPRRecomp/` (only while
  the game isn't running), then relink the console dev exe (`rm build/primerecomp.exe && make`).
