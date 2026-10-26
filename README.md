# Willow

A fan static recompilation of *Cotton: Fantastic Night Dreams* for the Sharp
X68000. The goal is a native Windows and Linux build, which also makes ports to
other platforms and, later on, mods practical. All of the game's 68000 code,
including its sound driver, is recompiled to native code, so there is no CPU
emulator. The only emulated parts are the two sound chips (YM2151 FM and
MSM6258 ADPCM), which are hardware with no code to translate.

Willow follows the [N64Recomp](https://github.com/N64Recomp/N64Recomp) model.
This repository contains only tools, runtime code and patches. It contains no
game code, graphics, sound or disk images. You supply your own copy of the game,
and the tools read it on your machine at build time.

## Please play the official game

Willow is a fan project, made for fun. It is not a substitute for
**Cotton Reboot!**, the official remake, which includes the original X68000
version of the game alongside its HD remake and runs on today's platforms. If
you enjoy Cotton, please support the people who make it by buying and playing
Cotton Reboot!:

- **PC:** [Steam](https://store.steampowered.com/app/1656820/)
- **Nintendo Switch:** [Nintendo eShop](https://www.nintendo.com/us/store/products/cotton-reboot-switch/)
- **Nintendo Switch 2:** [Nintendo eShop](https://www.nintendo.com/us/store/products/cotton-reboot-switch-2/)
- **PlayStation 4:** [PlayStation Store](https://store.playstation.com/en-us/product/UP8012-CUSA26437_00-5547337729150443)
- **PlayStation 5:** [PlayStation Store](https://store.playstation.com/concept/10018968)
- **Nintendo Switch 2 and PlayStation 5, physical editions:** [Clear River Games](https://shop.clearrivergames.com/collections/cotton-reboot)

Willow is not affiliated with or endorsed by Success, the owner of Cotton, or
by any of the game's publishers. All trademarks belong to their owners.

## What you need

- Your own dump of *Cotton: Fantastic Night Dreams* for the Sharp X68000
  (Electronic Arts Victor, 1993), two 2HD floppies, as `.dim` or `.xdf` images.
  Willow supports this one release only. `verify` tells you whether your dump
  matches it.
- Python 3.11 or newer. The tools use only the standard library.
- CMake 3.24+, a C/C++ compiler (GCC or Clang; MinGW-w64 on Windows) and Ninja,
  to build the game. Python tests that need a compiler are skipped without one.

## Getting started

```sh
cp config.example.toml config.local.toml   # then point it at your disk images
python -m tools.willow verify
```

Run every command from the repository root.

| Command   | What it does |
|-----------|--------------|
| `verify`  | Checks your disks against the supported release and prints a summary. |
| `ls`      | Lists every file on both disks. |
| `extract` | Writes every file to `build/disks/`, and the unpacked LZX executables (with entry point, section sizes and relocation offsets) to `build/unpacked/`. |
| `analyze` | Maps the code of each program in `programs.toml` and writes labeled listings to `build/analysis/` ([how it works](docs/analysis.md)). |
| `recomp` | Translates each program to C in `build/recomp/` ([how it works](docs/codegen.md)). |

Every command checks your disks first and stops with an explanation if they are
not the supported release. `--config <file>` uses a config other than
`config.local.toml`.

To build the game itself ([details](docs/runtime.md)):

```sh
git submodule update --init
python -m tools.willow extract
python -m tools.willow recomp
cmake -S . -B build/cmake -G Ninja
cmake --build build/cmake
```

The result is `build/cmake/runtime/willow` (`willow.exe` on Windows). The game
is playable, with music and voices.

### Controls

| Action | Keyboard | Gamepad |
|---|---|---|
| Move | Arrow keys or numpad 8/2/4/6 | D-pad or left stick |
| Insert coin (title), shoot | Z or Space | A (south), X (west) or Back |
| Start, magic | X | B (east), Y (north) or Start |

Cotton reads the X68000 keyboard directly. Its buttons are SPACE/XF1 and XF2, and
PC keyboards have no XF keys, so Z and X stand in for them. The other keys map to
the X68000 key in the same place (F11 is HELP, Pause is BREAK). A second gamepad
drives the second joystick port.

**Saving:** as on the X68000, Cotton saves only when told to. Press left or right
on the title screen for OPTIONS. On SYSTEM, pick SAVE and press shot to save the
options and rankings, or pick AUTOSAVE to have them saved after every game (LOAD
and RESET bring back the saved or the default ones). Rankings you don't save are
gone when you quit. TRACE MODE SAVE keeps a replay of your last game, and PLAY
shows it. Saves go to a `saves` folder next to the extracted disks
(`build/saves`), never onto the disks themselves.

**Speed:** game time follows the cycles a 10 MHz 68000 would have spent, so busy
scenes slow down as they did on the X68000. This is new and still being checked
against the original. Loading takes time too, from a model of the floppy
drives run three times faster than the real ones: Cotton reads both disks into
memory at startup, so the EA logo stays up with the boot jingle for about 30
seconds, and the title appears about 43 seconds after launch.

## How the version lock works

`version.toml` pins the release with SHA-256 hashes. It holds no game data.

- **Identity:** the hash of the main executable `cot.r` after LZX unpacking.
- **Assets:** the hash of every file on both disks. The one exception is `user/`
  on the data disk, which holds the high-score table and replay data that the
  game writes back, so it differs from one copy to the next.

Hashes are taken over file contents, not over the image file. A `.dim` or a
`.xdf` of the same disks passes, and so does a disk that differs only in unused
sectors.

## Status

Early tooling. Done so far:

- [x] DIM/XDF image reader and Human68k FAT12 filesystem (`tools/willow/disk.py`)
- [x] LZX 0.42 unpacker, recovered from the self-extracting stub (`tools/willow/lzx.py`)
- [x] Version lock and the `verify` / `ls` / `extract` tools
- [x] Sound driver interface mapped ([`docs/sound-driver.md`](docs/sound-driver.md))
- [x] 68000 decoder and control-flow analysis of `cot.r` and `ctnsnd.r` ([`docs/analysis.md`](docs/analysis.md))
- [x] Code generator: 68000 to C, compiling warning-free ([`docs/codegen.md`](docs/codegen.md))
- [x] Runtime core: memory map, interrupts, Human68k and IOCS calls, the boot chain ([`docs/runtime.md`](docs/runtime.md))
- [x] Video: graphic, text, sprite and BG layers, drawn line by line with raster interrupts ([`docs/runtime.md`](docs/runtime.md#video))
- [x] Sound: YM2151 (ymfm), MSM6258 and its DMA channel ([`docs/runtime.md`](docs/runtime.md#sound))
- [x] Input: keyboard (as the X68000 keyboard) and gamepads (as joysticks)
- [ ] Run at the right speed
- [ ] Patches and mod hooks

## Contributing

- Paths to user-supplied files come only from `config.local.toml`. Never hardcode them.
- Never commit game data. That includes disk images, extracted files, and code or assets copied out of them.
- Docs refer to the game only as "provide your own dump of" the supported release.
  Don't add instructions for obtaining or dumping it.
- No CPU emulation. All 68000 code is statically recompiled. Only the sound chips
  are emulated, at the register level, through third-party cores pinned to exact
  commits.

Run the tests with `python -m unittest discover -s tests -t .`. They build
synthetic disks and don't need the game.

## License

Willow's own code is released under the [MIT License](LICENSE). The license
covers only what is in this repository. *Cotton: Fantastic Night Dreams* and
*Cotton Reboot!* belong to their owners, and third-party code, such as the sound
chip cores, keeps its own license.
