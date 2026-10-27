# Settings and mods

Players change Willow through a settings file, and anyone can make drop-in mods
that replace the game's data files or patch its programs. This is the design;
the status of each part is at the end.

## Where things live

Next to the extracted disks (`build/` by default), beside the saves:

| Path | What |
|---|---|
| `build/willow.toml` | The settings. `willow.exe` writes a commented one with the defaults the first time it runs. |
| `build/mods/<id>/` | One folder per mod. A mod's id is its folder name. |
| `build/saves/` | What the game writes (scores, replays), as now. |

## Settings

`willow.toml` is a TOML file. Missing keys keep their defaults, and unknown keys
are reported, so a typo doesn't pass silently.

```toml
[options]
loading = "fast"      # "real" (the floppy drives' speed), "fast" (3x), "instant"
sprite_limit = true   # false shows every sprite, past the hardware's 32 a line
fullscreen = false
smooth = false        # smooth scaling instead of sharp pixels

[mods]
enabled = ["english", "more-lives"]  # in load order: a later mod wins
```

`willow_dump` ignores `willow.toml` so that runs stay repeatable;
`--settings FILE` gives it one, and `--mods DIR` says where that file's mods
are (by default the `mods` folder beside the disks).

## Mods

A mod is a folder:

```
build/mods/english/
  mod.toml            name, version, authors, description, patches
  files/system/...    replacement files for the system disk (drive A:)
  files/data/...      replacement files for the data disk (drive B:)
```

```toml
name = "English translation"
version = "1.0"
authors = ["Someone"]
description = "Translates the story scenes."
```

`name` is expected; the others are optional. Willow logs each mod it uses, with
its name, version and authors. Problems are reported when Willow starts (in a
window in `willow.exe`, in the log in `willow_dump`) and the game starts anyway:
a mod with no folder or no readable `mod.toml` is left out, and lesser problems,
such as a key Willow doesn't read, leave it in. A mod listed twice in
`mods.enabled` loads where it first appears.

A mod contains only its author's own work: new files, new bytes, and offsets.
It never contains data copied out of the game. Where a patch wants to make sure
it lands on the right bytes, it carries their SHA-256 hash, not the bytes
(the same idea as the version lock).

### Replacing files

When the game opens a file, Willow looks in the save folder first (files the
game wrote), then in the enabled mods from last to first, then on the extracted
disks. Names match case-insensitively, as on Human68k, in the mods' folders
too. A mod can also add files the disks don't have. Writes still go only to the
save folder, starting from the file as the game sees it, so a mod's files are
never changed. Only files the game reads from disk while running can be
replaced: the programs (`cot.r` and the others) are recompiled from the locked
release and always load from the disk, so changing them takes patches, and a
mod file named like a program is reported and not used.

### Patches

```toml
[[patch]]
program = "cot.r"        # a program from programs.toml, by file name
offset = 0x0123a         # into the program as it runs (after its self-decryption)
bytes = "7003 4e71"      # the new bytes, in hex
replaces = "9f86d08..."  # optional: SHA-256 of the bytes being replaced
```

Offsets are fixed because Willow supports exactly one release. Patches inside
`cot.r`'s self-decrypting startup are refused, since its loaded and running
bytes differ there.

A patch is one of two kinds, which Willow works out itself:

- **Data patches** change bytes the game only reads: tables, constants, text.
  The runtime applies them when it loads the program, so turning the mod on or
  off takes effect at the next start.
- **Code patches** change instructions, which are compiled into the game. The
  recompiler applies every enabled mod's patches before it analyzes a program,
  so new code a patch reaches is found too, and any patch that ends up covering
  an instruction is compiled in. Turning such a mod on or off means running
  `python -m tools.willow recomp` and rebuilding.

The build records which code patches it contains and where the code is. At
startup Willow checks the enabled mods against that, and if a code patch has
been added, removed or changed since the build, it says which mod and asks for
a rebuild rather than run a game that doesn't match the mod list.

## Status

1. **Settings file:** done. `willow.toml` with loading speed, sprite limit,
   fullscreen and smoothing (`runtime/src/settings.*`, read with the small TOML
   reader in `runtime/src/toml.*`).
2. **Mods and file replacement:** done. `mods.enabled`, `mod.toml` and
   replacement files (`runtime/src/mods.*`, and the layers in
   `runtime/src/filesystem.*`). A mod with patches is used without them for now.
3. **Patches:** data patches at load, code patches in the recompiler, the build
   check.
4. **For mod authors:** a guide with a worked example.

Key remapping and an in-game menu for all of this can follow.
