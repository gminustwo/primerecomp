# MPR Recomp

A static recompilation of *Metroid Prime Remastered* (Nintendo Switch, 2023, v1.0.0, title ID `010012101468C000`)
into a native Windows application. **Early bring-up: it does not boot yet.** See [PROGRESS.md](PROGRESS.md).

The game's AArch64 code is translated to C ahead of time and compiled for the host. Its system calls, services and
NVN graphics calls are implemented natively (Vulkan on Windows and Linux, Metal on macOS). No emulator runs.

> [!NOTE]
> **No game files are included.** This repository holds no game code, assets, keys or firmware, and nothing built
> from them. Everything derived from the game stays in `private/` (gitignored). You need your own legally obtained
> copy of Metroid Prime Remastered v1.0.0, with no updates applied.

## Based on LAS Recomp

This project is a fork of [LAS Recomp](https://github.com/peiikai/LASRecomp) by Peiikai and contributors (MIT),
the static recompilation of *The Legend of Zelda: Link's Awakening*, starting from upstream commit `b846e72`. The
generator (`tools/gen.py`, `tools/lifter.py`), the Horizon kernel and services, and the NVN-to-Vulkan/Metal
renderer come from there, and the full upstream history is kept. Upstream is the `lasrecomp` git remote, so its
fixes can be cherry-picked.

Link's Awakening's game code (its instruction patches, the hooks into its memory under `runtime/hle/`, the
randomizer and its UI) has been removed. Some of LA's presentation features (button prompt textures, cosmetics) and
its names (`lasrecomp`, `LARECOMP_*`) are still in the tree; see [PROGRESS.md](PROGRESS.md).

## Layout

    private/            your game files and everything made from them (gitignored)
      exefs/            main, sdk, subsdk0, main.npdm (from your own NSP)
      generated/        tools/gen.py output
    tools/              generator, lifter, analysis scripts
    runtime/            kernel, services, loader, dispatcher, NVN renderer, UI
      hle/game.c        what the runtime asks about the running game (this is where MPR-specific code goes)

## Generating the game code

    python -m venv .venv && .venv/Scripts/python -m pip install lz4 capstone unicorn
    cd tools
    ../.venv/Scripts/python gen.py sdk  ../private/exefs/sdk  0x7006000000 ../private/generated/sdk  --simd c
    ../.venv/Scripts/python gen.py main ../private/exefs/main 0x7000000000 ../private/generated/main --simd c

Building and running follow [docs/BUILD-windows.md](docs/BUILD-windows.md) (inherited from LAS Recomp; not yet
updated for MPR).

## License

MIT, as upstream. See [LICENSE](LICENSE).
