"""Hand-written replacements for single guest instructions, keyed by guest address (gen.py applies them while lifting).

Metroid Prime Remastered v1.0.0 (main at 0x7000000000).
"""

# The simulation step. The game builds its 1/60 s tick as `movz wN, #0x8889; movk wN, #0x3c88, lsl #16` (0x3c888889)
# wherever it needs it; at each site that means one simulation tick or one frame, the movk instead takes the step for
# the chosen frame rate (runtime/hle/game.c rt_sim_step_bits: 1/60, or 1/120 at 120 fps). Left alone: the motion
# sensor's 60 Hz sample period (CNXSixAxisAnalysis::UpdateMotion, 0x7000359e50).
STEP_SITES = {
    0x70000fe264: (8, False),   # CGraphicsServerDevice::BeginTargetFrame
    0x70000fe4a0: (8, False),   # CGraphicsServerDevice::GetRenderFrameIntervalTime
    0x70000ff2f8: (9, False),   # CGraphicsServerDevice::TickMainThreadMaintenance
    0x70003c81a0: (8, False),   # CGameArchitectureSupport::GetSimulationTimeStep
    0x70003c81b0: (8, False),   # CGameArchitectureSupport::GetTotalSimulationTimeStep
    # 0x70003c8234 (CGameArchitectureSupport::Update: the frame's elapsed time, clamped to one tick): below
    0x70003c8308: (27, False),  # CGameArchitectureSupport::Update: the tick
    # 0x70003c8350 (CGameArchitectureSupport::Update: minus one tick, 0xbc888889, each pass of the tick loop): below
    0x70006ce284: (8, False),   # NPlayerMovementHelpers::ComputeFramesPerBeatForBPM
    0x7000b30424: (8, False),   # CGameRenderJob::RenderGamePass
    0x7000b7c470: (8, False),   # CProjectileWeapon::GetTickTime
    0x7000c4e7d8: (8, False),   # CAnimDataMP1::OnUserEvent
    0x7000c4e7f8: (22, False),  # CAnimDataMP1::OnUserEvent
    0x7000c5abd4: (9, False),   # CStateManagerGameLogicMP1::CStateManagerGameLogicMP1
    0x7000cb69e0: (8, False),   # CAimingCursorMP1::Update
    0x7000cb6a24: (8, False),   # CAimingCursorMP1::Update
    0x7000fa1474: (8, False),   # CGameRenderJobMP1::RenderPauseScreenElements
    0x7000fa18bc: (20, False),  # CGameRenderJobMP1::RenderPauseScreenElements
    0x700101e560: (9, False),   # CNewFlameThrowerMP1::AcceptScriptMsg
}


def _step(gen, a):
    rd, neg = STEP_SITES[a]
    return f'{{ extern unsigned rt_sim_step_bits(int); c->x[{rd}] = rt_sim_step_bits({int(neg)}); }}'


PATCHES = {a: _step for a in STEP_SITES}

# CStateManager::BeginFrame (every gameplay frame; it also steps a running autosave): `mov x19, x0`, then the
# runtime's debug autosave (runtime/hle/game.c rt_debug_autosave, LARECOMP_AUTOSAVE_AT) - saving tested without
# reaching a save station.
PATCHES[0x7000b02dcc] = lambda gen, a: '{ c->x[19] = c->x[0]; extern void rt_debug_autosave(Cpu *); rt_debug_autosave(c); }'

# CPlayerMP1::UpdateCameraBob (each gameplay frame): `sub sp, sp, #0x90` after the runtime notes the player and the
# state manager (runtime/hle/game.c rt_player_frame: LARECOMP_POS_LOG, LARECOMP_TELEPORT).
PATCHES[0x7000cf315c] = lambda gen, a: '{ extern void rt_player_frame(Cpu *); rt_player_frame(c); c->sp -= 0x90; }'

# Areas (runtime/hle/game.c rt_area_load): CGameAreaManager::BeginAreaLoad's `mov x29, sp` (the area's CObjectId at
# [x1], the load type in w2) logs each area the game loads.
PATCHES[0x70003e4bec] = lambda gen, a: ('{ c->x[29] = c->sp; extern void rt_area_load(Cpu *); rt_area_load(c); }')

# CCameraManager::CheckForCinematicSkip: `tbz w0, #0` on the skip button's state; with LARECOMP_SKIP_CINEMATICS the
# button counts as pressed, so each cutscene (all skippable then: runtime/hle/game.c) ends on its first frame.
PATCHES[0x70004bd840] = lambda gen, a: ('{ extern int rt_skip_cinematics(void); if (rt_skip_cinematics()) c->x[0] |= 1; '
                                        'if ((c->x[0] & 1) == 0) goto L_70004bd788; }')

# CWorldStateMP1::SetCurrentArea (each gameplay frame, from GameMainLoop): `stp x1, x2, [x0, #0x10]` after the runtime
# notes the world state (runtime/hle/game.c rt_world_state: LARECOMP_SAVE_AREA sets its desired area for a save).
PATCHES[0x7000c82748] = lambda gen, a: ('{ extern void rt_world_state(Cpu *); rt_world_state(c); '
                                        'ST64(c->x[0] + 0x10, c->x[1]); ST64(c->x[0] + 0x18, c->x[2]); }')


# CGameArchitectureSupport::Update's step sites with the frame's timing for the runtime (runtime/hle/game.c
# rt_frame_step: LARECOMP_TICK_LOG): the elapsed time it measured (s8) and each pass of its tick loop.
PATCHES[0x70003c8234] = lambda gen, a: '{ extern unsigned rt_frame_step(Cpu *); c->x[8] = rt_frame_step(c); }'
PATCHES[0x70003c8350] = lambda gen, a: '{ extern unsigned rt_tick_pass(void); c->x[8] = rt_tick_pass(); }'
# ... and the tick body itself (`fmov s0, w27`: one simulation tick ran)
PATCHES[0x70003c8328] = lambda gen, a: ('{ extern void rt_tick_body(void); rt_tick_body(); '
                                        'c->v[0] = (__uint128_t)(uint32_t)c->x[27]; }')

# CMovieList::Update(float dt) (the movies' clock): `mov x29, sp` after the runtime logs dt (s0; LARECOMP_TICK_LOG)
PATCHES[0x70003669f4] = lambda gen, a: '{ c->x[29] = c->sp; extern void rt_movie_dt(Cpu *); rt_movie_dt(c); }'
