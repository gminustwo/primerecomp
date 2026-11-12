# RecompLauncher

PrimeRecomp supplies its own setup and settings profile. All implementation changes are in
PrimeRecomp: no additional RecompLauncher changes or Linux download-format changes are required.
The integration targets these RecompLauncher interfaces:

- [PR #12](https://github.com/elripalda/RecompLauncher/pull/12): discovery of the manifest embedded
  in a standalone AppImage. Full Linux integration needs this feature.
- [PR #22](https://github.com/elripalda/RecompLauncher/pull/22): named game inputs and conditional,
  shared keys. The manifest's Easy Setup flow, including launcher-managed key selection, needs
  this feature on Windows and Linux.

Both PRs were open when this integration was prepared. Older launcher builds do not provide the
complete flow. They can still launch PrimeRecomp manually after setup in its own start screen.
A future launcher release containing both interfaces is the compatibility requirement; no version
number is guessed before those features ship.

## Setup

Choose your own **Metroid Prime Remastered v1.0.0, without updates**:

- An extracted folder holding `exefs` and `romfs` needs no keys. PrimeRecomp validates the main
  module's build ID and the required game files before building its code.
- An **NSP or XCI** activates the manifest's `prod.keys` input. RecompLauncher can reuse a stored
  keyset; PrimeRecomp validates whether it can actually decrypt the selected game.
- Rights-managed packages use their embedded ticket, or `title.keys` beside `prod.keys`.
  A package without usable title rights fails setup with a diagnostic; title keys are not required
  for every NSP/XCI. The current named-input contract cannot conditionally ask for title keys
  based on a rights ID discovered during decryption, so the manifest does not demand them
  universally. Use existing title-key discovery or the explicit CLI option below for that case.

Package setup leaves the NSP/XCI where you selected it and reads it during play. Keep it available.
Extracted files are copied/linked into PrimeRecomp's data folder. Setups and compilation can take
several minutes; the manifest allows up to one hour. Cancelling preserves saves, and retrying can
reuse validated staged files. An interrupted code build restarts compilation from those files.

Windows uses its installation folder for settings, saves and prepared game files. Linux uses
`$XDG_DATA_HOME/primerecomp` (normally `~/.local/share/primerecomp`). The manifest passes these
paths explicitly to both preparation and launch so the working directory cannot change them.
Preparation creates a full default INI only when none exists, including the Steam Deck's first-run
preset. Existing settings are retained. RecompLauncher edits the same INI that F1 edits.

Play goes directly into the game. Big Picture initially opens fullscreen without saving that
session default over the user's window preference. F11 or the in-game Fullscreen control still
changes and saves an explicitly chosen preference. Controller prompts continue to follow the
controller through PrimeRecomp's existing Auto setting.

Only the `save` folder is declared for backups/cloud saves. Keys, game files, caches, generated code
and `extracted/romfs.src` are excluded. The latter contains decryption material; do not share it as a
save or diagnostic attachment. Setup logs contain messages and paths, never key values.

## Command line

```text
primerecomp --prepare --game=<folder|NSP|XCI> [--keys=<prod.keys>] [--title-keys=<title.keys>]
primerecomp --play
primerecomp --preload-shaders
```

The Windows executable and Linux AppImage accept the same options. Both `--game=PATH` and
`--game PATH` forms work. Quote an entire argument when a path has spaces. Input paths are made
absolute before changing to the data folder. `--data-root=PATH` selects an explicit data folder for
both setup and play; existing `LARECOMP_DATA_ROOT` development/test overrides still work when no
CLI override is provided.

`--prepare` opens no window and returns 0 only when the prepared game is ready. It prints flushed
`progress <0..1> <message>` lines on stdout and writes detailed English diagnostics to
`primerecomp-setup.log` in the data folder. Invalid arguments return 2; setup failures return 1.
A closed progress pipe cancels the operation and ends compiler subprocesses. No game content,
keys or compilation output is downloaded by this command beyond the project's existing toolchain.
An explicit `--keys` path is authoritative; without one, existing package-adjacent/emulator key
discovery applies. `--title-keys` supplies a separate file without copying it beside the game or
`prod.keys`; ticket lookup remains first. Selected source/key paths and metadata are recorded
locally to identify repeat setup, without recording the keys' contents.

## Packaging and validation

`tools/recomplauncher.py` generates both profiles from one definition. `make dist` places the
Windows profile beside `primerecomp.exe`. Linux packaging places a regular `/recomplauncher.json`
at the AppDir root and disables extended attributes in SquashFS for compatibility with the reader
in PR #12. AppImage program entries name the release image; that reader rebinds them when renamed.
The Linux download remains a standalone AppImage and its updater format is unchanged.

Run `python tools/test_recomplauncher.py` for manifest contracts, optionally passing `--schema`
with PR #22's JSON schema. Compile `tools/test_launcher_cli.c` together with
`runtime/launcher_cli.c` and pass the result with `--parser`; pass the runtime or AppImage with
`--exe` for headless smoke tests using only synthetic files and an isolated data/save folder.
The RecompLauncher integration workflow builds Windows and Linux in GitHub Actions and uploads
review artifacts. Its Windows artifact is a runtime smoke-test folder, without the setup generator
or compiler; it is not a replacement for the complete `make dist` release.

Tests without real game content do not prove successful import/decryption, live game launch,
controller behavior or save synchronization. Those require a user's own game and supported
hardware. No verified-dump badge or clean-content claim is supplied.
