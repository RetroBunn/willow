# Willow

Fan static recompilation of Cotton: Fantastic Night Dreams (Sharp X68000) to
native Windows/Linux, on the N64Recomp model. The repo ships only tools,
runtime code and patches. Users supply their own dump at build/run time.

## Hard rules

1. Paths to user-supplied files come only from the gitignored `config.local.toml`
   (loaded by `tools/willow/config.py`). Never hardcode them, not even in tests or scripts.
2. Version lock: exactly one release. Its identity is the SHA-256 of the main
   executable `cot.r` after LZX unpacking, plus a per-file hash manifest of both
   disks. Both live in `version.toml`, which holds hashes only. Every tool must get
   the game through `lock.load_game()`, which refuses mismatches with a clear error.
   Never add a tool that bypasses it. Never commit a generator that writes
   `version.toml` from whatever disks happen to be configured.
3. Never write dumping, key-extraction, or console-modding instructions into the
   repo or docs. Docs only say "provide your own dump of" the supported release.
4. No CPU emulation. All 68000 code, the game and the sound driver alike, is
   statically recompiled. The only thing emulated is hardware with no code to
   translate: the YM2151 (FM) and MSM6258 (ADPCM) sound chips, at the register
   level, through third-party cores pinned to exact commits, plus small in-house
   models of the DMA controller and PPI behavior the sound driver relies on.

Never commit game data: disk images, extracted files, unpacked code, or tables
and assets copied out of them. `build/` is gitignored for this reason.

## Facts about the target

- Two 2HD floppies, 1024-byte sectors, Human68k FAT12. The system disk (drive A:,
  marker file `cotsys`) boots a custom IPL into HUMAN.SYS. The data disk
  (drive B:, marker `cotdat`) carries a PC-98 style BPB. The disks are identified
  by marker file, not by position in the config.
- Boot chain: `config.sys` runs `ealogo.r` (the logo), then `SHELL = bootcot.r`
  launches `cot.r`. `ctnsnd.r` is the sound driver (not packed).
- `cot.r`, `ealogo.r` and `cotins.r` are packed with LZX 0.42. The format is
  documented at the top of `tools/willow/lzx.py`.
- `user/` on the data disk is written by the game (scores, replay) and is excluded
  from the lock.
- Sound: `cot.r` `_EXEC`s `ctnsnd.r`, which stays resident and is driven only
  through `trap #0` (a command queue). Its tick is YM2151 timer B (~60 Hz). Full
  contract in `docs/sound-driver.md`.
- `programs.toml` lists the programs to recompile, with roots and decryption
  stages (offsets only). `cot.r`'s startup decrypts two stages of itself: the
  recompiler translates the decrypted `code` view, while the runtime must load
  the original `image` bytes. The header's text size is only a hint; some code
  sits in the data section. See `docs/analysis.md`.

## Working

- Run tools from the repo root: `python -m tools.willow verify|ls|extract|analyze|recomp`.
- Generated C targets `runtime/include/willow/m68k.h`. Calls push real return
  addresses and returns go through a per-function resume dispatcher; keep that
  contract when changing either side (`docs/codegen.md`).
- Time is the 68000's: generated code adds each instruction's cycles
  (`tools/willow/timing.py`) to `ctx->cycles`, and the runtime's clock is that
  count, paced to the host only in `willow.exe`. Game speed and slowdown depend
  on those counts, so keep them faithful to the 68000 manual. Native code that
  stands in for slow work spends its time with `Machine::busy`: Human68k's file
  calls charge the floppy drive's time (`Floppy`), and `_EXEC` the LZX
  unpacker's (`unpack_cycles`).
- Tests: `python -m unittest discover -s tests -t .`. They use synthetic disks only.
- Python 3.11+, standard library only.
- Runtime: C++17, SDL 3.4.18 (pinned by URL and hash in `runtime/CMakeLists.txt`),
  ymfm (git submodule in `external/`, pinned commit; `git submodule update --init`),
  CMake. Third-party code goes in as pinned submodules. Build with `cmake -S . -B build/cmake -G Ninja && cmake --build build/cmake`.
  Runtime tests: `ctest --test-dir build/cmake` (headless, stand-in programs only).
- All 68000 code runs on one game thread; interrupts are delivered when generated
  code polls (`M68K_POLL`: the cycle deadline or `m68k_pending`). Keep emulated
  state on that thread (`docs/runtime.md`).
- `willow_dump` runs the game headlessly for N frames and writes PNG snapshots and
  a log to `build/dump`. A "no recompiled function at X" stop means analysis
  missed a code shape (computed jump, table, unrolled loop): fix the analyzer or
  declare it in `programs.toml`, never patch around it in the runtime.
- Debugging aids are `WILLOW_*` environment variables, read in one place
  (`Debug::from_environment`, runtime/src/machine.*) and listed in
  docs/runtime.md, "Debugging". Add new ones there, not as `getenv` calls elsewhere.
