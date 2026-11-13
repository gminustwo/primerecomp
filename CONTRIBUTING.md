# Contributing to PrimeRecomp

Thanks for wanting to help! Bug reports, testing on different hardware and code contributions are all welcome.

## The one hard rule: no Nintendo files

Nothing owned by Nintendo may ever be committed or attached to an issue or pull request:

- no copies of the game in any form
- no extracted game files (`extracted/`), no generated code (`generated/`), no game code libraries (`primegame.*`)
- no game assets of any kind: textures, models, music, text, level files - also not "just one file for testing"

Pull requests that include Nintendo files can't be merged. Screenshots and short clips of the game in bug reports are fine.

### Bug reporting; Feature requests

Please report bugs and request features on the [Issues tabs](https://github.com/gminustwo/primerecomp/issues). For a bug, these help the most:

- **Screenshots, a GIF or a short video** of the problem
- **The details:** what happened, what you were doing and where in the game, and your system (Windows, macOS, Linux or
  Steam Deck; CPU and GPU, or your Mac's model)
- **A copy of your save file:** zip up the `save` folder (Windows: in the PrimeRecomp folder; macOS:
  `~/Library/Application Support/io.github.primerecomp/save`; Linux and Steam Deck: `~/.local/share/primerecomp/save`)
- `primerecomp.log`, if you can, from the folder that holds `save`. It always holds the latest run, and
  `primerecomp.previous.log` the run before. It notes your settings and what you change, each room you enter and
  your items there, so it says where you were when something went wrong (nothing leaves your PC unless you send it).

For a graphics problem, you can also press **F9** at the spot: it saves a capture of that frame in
`build/f9_<frame>/` in that same folder (zip it up). **F8** notes your position in `positions.txt`.

## Building

See [docs/BUILD-windows.md](docs/BUILD-windows.md) for Windows, [docs/linux.md](docs/linux.md) for Linux and
[docs/macos.md](docs/macos.md) for macOS. In short: set up your own v1.0.0 game files on the start screen, or put them
in `extracted/`; `make gen` once to recompile the game code from them, then `make -j`.

## Project layout

- `tools/gen.py` - the recompiler: translates the game's executables into C (`generated/`, never committed).
  `tools/patches.py` holds code patches applied during generation; every patch site checks the original instruction
  word, so a wrong address fails loudly instead of silently.
- `runtime/` - the host side:
  - `kernel.c`, `loader.c`, `services/` - the Switch OS replacement
  - `hle/` - host implementations of SDK functions (input, movies, heaps, ...) and the game-specific features
    (`game.c`: frame rate, field of view, mouse aim, ultrawide)
  - `nvn/` - the graphics layer: command recording (`cmdbuf.c`), the Vulkan renderer (`vulkan/`, on every system:
    macOS through MoltenVK), the shader translator (`maxwell/translate.c`)
  - `ui/` - the ImGui overlay menu and the start screen
  - `platform/` - what differs per OS (`windows.c`, `linux.c`, `macos.c`; `posix.c` for what Linux and macOS share)

## Testing your change

- Build cleanly on your platform (`make -j`). If you touch shared rendering code, say which OS / GPU you tested -
  MoltenVK (macOS) lacks some features desktop drivers have (geometry shaders, for one).
- Test in game, and look at the result: a fix isn't verified until you've seen it work.
- Scripted test runs: `tools/shot.sh <frame> [seconds]` runs the game with `LARECOMP_*` environment hooks (scripted
  input, teleports, skipped cutscenes) and saves a screenshot. Many debug hooks are documented where they're defined -
  grep for `getenv("LARECOMP_`. Give test runs a scratch save folder (`LARECOMP_SAVE_DIR`) so your own saves stay
  untouched.
- Per-boot state must be reset when the game reboots in place (back to the start screen): use an `RT_ON_RESET` hook
  (see `runtime/hle.h`).

## Code style

- Match the surrounding code: its naming, structure and comment density. Comments explain *why* (what the game does,
  what an address is, which scene or bug showed the problem), not what the next line does.
- Settings get short labels and a one-paragraph description, in the menu's own style.

## Pull requests

- Open them against `main`. Keep each focused on one change; small is easier to review.
- Describe what changed, why, and how you tested it (platform, GPU, screenshots).
- Don't include personal paths or information in code, comments or commit messages.

## License

By contributing, you agree that your contributions are licensed under the project's [MIT License](LICENSE).
